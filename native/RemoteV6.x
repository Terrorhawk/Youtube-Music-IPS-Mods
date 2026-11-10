// IPS player adapter. Private app selectors require physical validation on 8.21.3.
#import <UIKit/UIKit.h>
#import <Security/Security.h>
#import <objc/message.h>
#import <math.h>
#import <string.h>
#import <AVFoundation/AVFoundation.h>
#import <MediaPlayer/MediaPlayer.h>

static __weak id IPSPlayer;
static __weak id IPSNowPlaying;
static NSURLSession *IPSSession;
static BOOL IPSBusy, IPSKnownPlaying, IPSPlaying;
static NSUInteger IPSGeneration;
static double IPSClockOffset;
static NSString *IPSDesiredID, *IPSEndedID, *IPSEndedEvent;
static NSDate *IPSLastContact;
static NSTimeInterval IPSNextPoll, IPSProgressAt, IPSLoadAt, IPSFaultAt, IPSLastRemoteLoad;
static NSUInteger IPSFailures;
static double IPSLastPosition;
static NSString *IPSProgressVideo, *IPSFault, *IPSFaultID;
static BOOL IPSOfflineFallback, IPSAllowFallback;
static NSInteger IPSHTTPStatus;
static BOOL IPSInactive;
static BOOL IPSQueueInternalLoad, IPSQueueImmediate;
static BOOL IPSActualPlaying(BOOL *playing);
static BOOL IPSHoldActive(void);
static void IPSDualReturnToUI(void);
static void IPSOrdinaryLoadInfo(void);
static void IPSBindSurface(void);
static void IPSPublishPresentation(BOOL force);
static void IPSRememberMetadata(NSDictionary *command);
static void IPSStartPresentation(void);
static void IPSPresentationPulse(void);
static void IPSReleasePresentation(void);
static BOOL IPSPublishingSystemInfo;
static BOOL IPSLocalPlaybackPaused(void);
static BOOL IPSUserLoadBlocked(NSString *action);
static void IPSUserCommandWillApply(NSString *action);
static NSString *IPSActiveLoadCommand, *IPSPendingLoadCommand;
static double IPSLatestControlExpiry;
static BOOL IPSControlIsStale(NSDictionary *command);
static void IPSControlAccepted(NSDictionary *command);
static void IPSRecordCrossFailure(void);
static BOOL IPSCrossRunning(void);
static BOOL IPSShouldMute(id player);
static void IPSQueueCancel(BOOL failed);
static BOOL IPSQueueLoad(NSString *target, NSString *command);
static NSDictionary *IPSCrossStatus(NSDictionary *status);
static NSDictionary *IPSConfig;
static NSUInteger IPSFallbackIndex;
static UILabel *IPSOverlay;
static AVAudioPlayer *IPSEmergency;
static void IPSTick(void);
static void IPSMonitor(void);
static void IPSFetchSegments(void);
static void IPSRefreshOverlay(void);
static void IPSAfterCommand(NSString *action);
static void IPSAudioOnly(void);
static NSString *IPSConnection = @"Nog niet verbonden";

static NSUserDefaults *IPSD(void) { return NSUserDefaults.standardUserDefaults; }
static NSString *IPSSetting(NSString *key) { return [IPSD() stringForKey:[@"IPS." stringByAppendingString:key]] ?: @""; }
static BOOL IPSEnabled(void) { return [IPSD() boolForKey:@"IPS.enabled"]; }
static NSString *IPSEvent(void) { return IPSSetting(@"event").length ? IPSSetting(@"event") : @"default"; }
static BOOL IPSMatch(NSString *s, NSString *pattern) {
    return [s isKindOfClass:NSString.class] && [s rangeOfString:pattern options:NSRegularExpressionSearch].location != NSNotFound;
}
static id IPSObject(id target, NSString *name) {
    SEL selector = NSSelectorFromString(name);
    NSMethodSignature *sig = [target methodSignatureForSelector:selector];
    if (!sig || sig.numberOfArguments != 2 || sig.methodReturnType[0] != '@') return nil;
    return ((id(*)(id,SEL))objc_msgSend)(target,selector);
}
static BOOL IPSInvoke(id target, NSString *name) {
    SEL selector = NSSelectorFromString(name);
    NSMethodSignature *sig = [target methodSignatureForSelector:selector];
    if (!sig || sig.numberOfArguments != 2 || strcmp(sig.methodReturnType,@encode(void))) return NO;
    ((void(*)(id,SEL))objc_msgSend)(target,selector);
    return YES;
}
static id IPSTime(NSString *name) {
    SEL selector=NSSelectorFromString(name);
    NSMethodSignature *sig=[IPSPlayer methodSignatureForSelector:selector];
    if (!sig || sig.numberOfArguments!=2 || strcmp(sig.methodReturnType,@encode(CGFloat))) return NSNull.null;
    CGFloat v=((CGFloat(*)(id,SEL))objc_msgSend)(IPSPlayer,selector);
    return isfinite(v)&&v>=0 ? @(v) : NSNull.null;
}
static NSString *IPSVideo(void) {
    id value=IPSObject(IPSPlayer,@"currentVideoID") ?: IPSObject(IPSPlayer,@"contentVideoID");
    return [value isKindOfClass:NSString.class] ? value : @"";
}
static NSMutableDictionary *IPSKeyQuery(void) {
    return [@{(__bridge id)kSecClass:(__bridge id)kSecClassGenericPassword,
        (__bridge id)kSecAttrService:@"eu.interactiveplaylist.remote",
        (__bridge id)kSecAttrAccount:@"player-token"} mutableCopy];
}
static NSString *IPSToken(void) {
    NSMutableDictionary *q=IPSKeyQuery();
    q[(__bridge id)kSecReturnData]=@YES;
    CFTypeRef result=NULL;
    if (SecItemCopyMatching((__bridge CFDictionaryRef)q,&result)!=errSecSuccess) return @"";
    NSData *data=CFBridgingRelease(result);
    return [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding] ?: @"";
}
static BOOL IPSStoreToken(NSString *value) {
    NSMutableDictionary *q=IPSKeyQuery();
    NSDictionary *attrs=@{(__bridge id)kSecValueData:[value dataUsingEncoding:NSUTF8StringEncoding],
        (__bridge id)kSecAttrAccessible:(__bridge id)kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    OSStatus status=SecItemUpdate((__bridge CFDictionaryRef)q,(__bridge CFDictionaryRef)attrs);
    if (status==errSecItemNotFound) {
        [q addEntriesFromDictionary:attrs];
        status=SecItemAdd((__bridge CFDictionaryRef)q,NULL);
    }
    return status==errSecSuccess;
}

@interface IPSNetworkDelegate : NSObject <NSURLSessionTaskDelegate>
@end
@implementation IPSNetworkDelegate
- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task willPerformHTTPRedirection:(NSHTTPURLResponse *)response newRequest:(NSURLRequest *)request completionHandler:(void (^)(NSURLRequest *))handler {
    handler(nil); // Never forward the player token through redirects.
}
@end

static void IPSRequest(NSString *path, NSDictionary *body, void(^done)(NSDictionary *)) {
    NSUInteger generation=IPSGeneration;
    NSURL *base=[NSURL URLWithString:IPSSetting(@"url")];
    NSString *token=IPSToken();
    if (!IPSEnabled() || ![base.scheme isEqual:@"https"] || !base.host.length || token.length<24) { done(nil); return; }
    NSString *prefix=[base.absoluteString stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"/"]];
    NSURL *url=[NSURL URLWithString:[NSString stringWithFormat:@"%@/api/ips/v1/%@",prefix,path]];
    NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:url];
    request.timeoutInterval=6;
    [request setValue:[@"Bearer " stringByAppendingString:token] forHTTPHeaderField:@"Authorization"];
    [request setValue:IPSEvent() forHTTPHeaderField:@"X-IPS-Event"];
    [request setValue:IPSSetting(@"playerId") forHTTPHeaderField:@"X-IPS-Player-ID"];
    if (body) {
        request.HTTPMethod=@"POST";
        request.HTTPBody=[NSJSONSerialization dataWithJSONObject:body options:0 error:NULL];
        [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
    }
    [[IPSSession dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error) {
        NSInteger code=[(NSHTTPURLResponse *)response statusCode];
        id parsed=nil;
        if (!error && (code==200 || code==409) && data.length<=524288) parsed=[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
        NSDictionary *reply=[parsed isKindOfClass:NSDictionary.class] ? parsed : nil;
        dispatch_async(dispatch_get_main_queue(),^{
            if (generation!=IPSGeneration) return;
            IPSHTTPStatus=code;
            IPSConnection=reply ? @"Verbonden" : error ? @"Server niet bereikbaar" : [NSString stringWithFormat:@"Servermelding %ld",(long)code];
            if ([reply[@"serverTime"] isKindOfClass:NSNumber.class]) IPSClockOffset=[reply[@"serverTime"] doubleValue]-NSDate.date.timeIntervalSince1970*1000;
            done(reply);
        });
    }] resume];
}
static void IPSAck(NSString *identifier, NSString *result) {
    IPSRequest(@"ack",@{@"id":identifier,@"result":result},^(NSDictionary *reply){ IPSBusy=NO; });
}
static NSString *IPSJournalKey(void) {
    return [NSString stringWithFormat:@"IPS.journal.%@.%@",IPSSetting(@"url"),IPSEvent()];
}
static void IPSFadeCancel(void);
static void IPSFadeActivated(void);
static BOOL IPSLoad(NSString *videoID) {
    if (IPSCrossRunning() && !IPSQueueInternalLoad) IPSQueueCancel(YES);
    IPSFadeCancel();
    IPSDualReturnToUI();
    IPSAudioOnly();
    id delegate=UIApplication.sharedApplication.delegate;
    SEL selector=@selector(application:openURL:options:);
    NSMethodSignature *sig=[delegate methodSignatureForSelector:selector];
    if (!sig || sig.numberOfArguments!=5 || (strcmp(sig.methodReturnType,@encode(BOOL)) && strcmp(sig.methodReturnType,"c"))) return NO;
    for (NSUInteger i=2;i<5;i++) if ([sig getArgumentTypeAtIndex:i][0]!='@') return NO;
    NSURL *url=[NSURL URLWithString:[@"youtubemusic://watch?v=" stringByAppendingString:videoID]];
    return ((BOOL(*)(id,SEL,id,id,id))objc_msgSend)(delegate,selector,UIApplication.sharedApplication,url,@{});
}
static void IPSCommand(id value) {
    if (![value isKindOfClass:NSDictionary.class]) { IPSBusy=NO; return; }
    NSDictionary *command=value;
    NSString *identifier=command[@"id"], *action=command[@"action"];
    if (!IPSMatch(identifier,@"^[A-Za-z0-9_-]{1,96}$")) { IPSBusy=NO; return; }
    if(IPSUserLoadBlocked(action)){IPSBusy=NO;return;}
    NSString *journalKey=IPSJournalKey();
    NSMutableDictionary *journal=[[IPSD() dictionaryForKey:journalKey] mutableCopy] ?: NSMutableDictionary.dictionary;
    double now=NSDate.date.timeIntervalSince1970*1000+IPSClockOffset;
    // Expired commands cannot be delivered by the server again. Retain an extra
    // two minutes for clock correction/reconnect, then bound the local journal.
    for (NSString *old in journal.allKeys) {
        id entry=journal[old];
        if ([entry isKindOfClass:NSDictionary.class] && [entry[@"expiresAt"] doubleValue]+120000<now) [journal removeObjectForKey:old];
    }
    if (journal[identifier]) { IPSAck(identifier,journal[identifier][@"result"]); return; }
    NSString *result=@"unsupported";
    NSNumber *expires=command[@"expiresAt"];
    if (![expires isKindOfClass:NSNumber.class] || expires.doubleValue<=NSDate.date.timeIntervalSince1970*1000+IPSClockOffset) result=@"expired";
    else if (IPSControlIsStale(command)) result=@"expired";
    else if (journal.count>=2000) result=@"history_full";
    else {
        BOOL valid=[@[@"pause",@"resume",@"stop"] containsObject:action] || ([action isEqual:@"load"] && IPSMatch(command[@"videoId"],@"^[A-Za-z0-9_-]{11}$"));
        if (valid) {
            IPSControlAccepted(command);
            journal[identifier]=@{@"result":@"uncertain",@"expiresAt":expires};
            [IPSD() setObject:journal forKey:journalKey];
            [IPSD() synchronize];
            @try {
                IPSRememberMetadata(command);
                IPSUserCommandWillApply(action);
                if ([action isEqual:@"load"]) {
                    IPSPendingLoadCommand=identifier;
                    NSString *prior=IPSDesiredID;
                    if(![command[@"crossfade"] boolValue]) IPSOrdinaryLoadInfo();
                    IPSDesiredID=command[@"videoId"];
                    IPSEndedEvent=nil; IPSEndedID=nil; IPSKnownPlaying=NO; IPSLoadAt=NSDate.date.timeIntervalSince1970; IPSLastRemoteLoad=IPSLoadAt; IPSProgressAt=IPSLoadAt;
                    [IPSD() setObject:IPSDesiredID forKey:@"IPS.desiredVideoId"];
                    IPSQueueImmediate=[command[@"crossfadeImmediate"] boolValue];
                    result=([command[@"crossfade"] boolValue] ? IPSQueueLoad(IPSDesiredID,identifier) : IPSLoad(IPSDesiredID)) ? @"submitted" : @"unsupported";
                    if ([result isEqual:@"unsupported"]) { if([command[@"crossfade"] boolValue]) IPSRecordCrossFailure(); IPSPendingLoadCommand=nil;IPSDesiredID=prior; [IPSD() setObject:prior ?: @"" forKey:@"IPS.desiredVideoId"]; }
                } else {
                    NSString *method=[action isEqual:@"resume"] ? @"play" : @"pause";
                    if ([action isEqual:@"stop"]) { IPSDesiredID=nil; [IPSD() removeObjectForKey:@"IPS.desiredVideoId"]; }
                    result=IPSInvoke(IPSPlayer,method) ? @"submitted" : @"unsupported";
                }
            } @catch (NSException *exception) { result=@"unsupported"; }
        }
    }
    // Keep uncertain entries after a crash; never blindly replay an accepted load.
    if (journal.count<2000 || journal[identifier]) {
        journal[identifier]=@{@"result":result,@"expiresAt":expires ?: @0}; [IPSD() setObject:journal forKey:journalKey]; [IPSD() synchronize];
    }
    if ([result isEqual:@"submitted"]) {IPSAfterCommand(action);IPSPublishPresentation(YES);}
    IPSAck(identifier,result);
}
static NSDictionary *IPSStatus(void) {
    NSString *videoID=IPSVideo();
    NSMethodSignature *playingSignature=[IPSNowPlaying methodSignatureForSelector:@selector(isPlaying)];
    if (playingSignature && playingSignature.numberOfArguments==2 &&
        (!strcmp(playingSignature.methodReturnType,@encode(BOOL)) || !strcmp(playingSignature.methodReturnType,"c"))) {
        IPSPlaying=((BOOL(*)(id,SEL))objc_msgSend)(IPSNowPlaying,@selector(isPlaying)); IPSKnownPlaying=YES;
    }
    BOOL actualPlaying=NO;if(IPSActualPlaying(&actualPlaying)){IPSPlaying=actualPlaying;IPSKnownPlaying=YES;}
    if(IPSLocalPlaybackPaused()){IPSPlaying=NO;IPSKnownPlaying=YES;}
    NSString *state=IPSEndedEvent && [IPSEndedID isEqual:videoID] ? @"ended" : IPSKnownPlaying ? (IPSPlaying ? @"playing" : @"paused") : @"unknown";
    if (IPSDesiredID.length && videoID.length && ![IPSDesiredID isEqual:videoID]) state=@"unexpected_track";
    return IPSCrossStatus(@{@"videoId":videoID,@"desiredVideoId":IPSDesiredID ?: @"",@"position":IPSTime(@"currentVideoMediaTime"),
        @"duration":IPSTime(@"currentVideoTotalMediaTime"),@"playbackState":state,
        @"endedEvent":IPSEndedEvent ?: @"",@"endedVideoId":IPSEndedID ?: @"",
        @"appVersion":[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"unknown",
        @"adapterVersion":([NSBundle.mainBundle objectForInfoDictionaryKey:@"IPSBuildVersion"] ?: @"test06"),@"hasPlayer":@(IPSPlayer!=nil)});
}
#include "IPSFade.h"
#include "IPSCrossfadeProbe.h"
#include "IPSQueueCrossfade.h"
#include "IPSSurfaceBinding.h"
#include "RemoteV6Runtime.h"
#include "IPSPresentationV05.h"

static UIViewController *IPSPresenter(void) {
    for (UIWindow *window in UIApplication.sharedApplication.windows) {
        if (!window.isKeyWindow) continue;
        UIViewController *vc=window.rootViewController;
        while (vc.presentedViewController) vc=vc.presentedViewController;
        return vc;
    }
    return nil;
}
static void IPSShowScanner(UIViewController *presenter);
static void IPSChooseEmergency(UIViewController *presenter);
@interface IPSSettingsController : UITableViewController
@property UITextField *server, *event, *token, *deviceName;
@property UISwitch *enabled, *ads, *background, *sponsor, *normalization, *awake;
@property UISegmentedControl *minimumQuality;
@property UISwitch *statusOverlay, *fade, *queueCross;
@property UITextField *fadeInSeconds, *fadeOutSeconds, *crossSeconds;
- (void)transitionChanged:(UISwitch *)sender;
@end
@implementation IPSSettingsController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.title=@"IPS Player";
    self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel target:self action:@selector(cancel)];
    self.navigationItem.rightBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemSave target:self action:@selector(save)];
    self.server=[self field:IPSSetting(@"url") placeholder:@"https://ips.example.org"];
    self.event=[self field:IPSSetting(@"event") placeholder:@"Leeg = standaardevent"];
    self.token=[self field:@"" placeholder:IPSToken().length ? @"Opgeslagen - leeg laten om te behouden" : @"Koppelsleutel uit het beheer"];
    self.token.secureTextEntry=YES; self.deviceName=[self field:IPSSetting(@"deviceName").length ? IPSSetting(@"deviceName") : UIDevice.currentDevice.name placeholder:@"Device naam (bijvoorbeeld Terrorhawk)"];
    self.enabled=UISwitch.new; self.enabled.on=IPSEnabled();
    NSDictionary *settings=[IPSD() dictionaryForKey:@"YTMUltimate"];
    self.ads=UISwitch.new; self.ads.on=[settings[@"noAds"] boolValue];
    self.sponsor=UISwitch.new; self.sponsor.on=[IPSD() boolForKey:@"IPS.sponsorBlock"];
    self.normalization=UISwitch.new; self.normalization.on=[IPSD() boolForKey:@"IPS.normalization"];
    self.minimumQuality=[[UISegmentedControl alloc] initWithItems:@[@"Auto",@"Laag",@"Normaal",@"Hoog"]]; self.minimumQuality.selectedSegmentIndex=MIN(3,MAX(0,[IPSD() integerForKey:@"IPS.minimumAudioQuality"]/10));
    self.fadeInSeconds=[self field:[NSString stringWithFormat:@"%.1f",IPSFadeSeconds(@"IPS.fadeInSeconds")] placeholder:@"0 tot 8 seconden"]; self.fadeOutSeconds=[self field:[NSString stringWithFormat:@"%.1f",IPSFadeSeconds(@"IPS.fadeOutSeconds")] placeholder:@"0 tot 8 seconden"]; self.crossSeconds=[self field:[NSString stringWithFormat:@"%.1f",IPSCrossSeconds()] placeholder:@"1 tot 8 seconden"]; self.queueCross=UISwitch.new;self.queueCross.on=[IPSD() boolForKey:@"IPS.queueCrossfade"];self.fade=UISwitch.new; self.fade.on=[IPSD() boolForKey:@"IPS.fade"]; self.statusOverlay=UISwitch.new; self.statusOverlay.on=[IPSD() boolForKey:@"IPS.showStatusOverlay"];
    self.awake=UISwitch.new; self.awake.on=[IPSD() boolForKey:@"IPS.keepAwake"];
    self.background=UISwitch.new; self.background.on=[settings[@"backgroundPlayback"] boolValue];
    if(self.queueCross.on)self.fade.on=NO;
    [self.queueCross addTarget:self action:@selector(transitionChanged:) forControlEvents:UIControlEventValueChanged];
    [self.fade addTarget:self action:@selector(transitionChanged:) forControlEvents:UIControlEventValueChanged];
    [self transitionChanged:nil];
}
- (UITextField *)field:(NSString *)value placeholder:(NSString *)placeholder {
    UITextField *field=UITextField.new;
    field.text=value; field.placeholder=placeholder;
    field.autocapitalizationType=UITextAutocapitalizationTypeNone;
    field.autocorrectionType=UITextAutocorrectionTypeNo;
    field.clearButtonMode=UITextFieldViewModeWhileEditing;
    return field;
}
- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView { return 3; }
- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section { return section==0 ? 5 : section==1 ? 11 : 2; }
- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section { return section==0 ? @"Remoteverbinding" : section==1 ? @"Afspelen" : @"Koppeling en noodmuziek"; }
- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section==0) return [NSString stringWithFormat:@"%@\nLaatste servercontact: %@\nMislukte contacten: %lu\nLokale fallback: %@\nLiveshow: %@", IPSConnection, IPSLastContact ? [NSDateFormatter localizedStringFromDate:IPSLastContact dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterMediumStyle] : @"nog geen", (unsigned long)IPSFailures, IPSOfflineFallback ? @"actief" : @"niet actief", IPSConfig ? ([IPSConfig[@"online"] boolValue] ? @"uit" : @"actief") : @"onbekend"];
    if (section==1) return [NSString stringWithFormat:@"Crossfade en fade-in/out zijn alternatieven.\n\nAfspeelstatus: %@",IPSCrossInfo];
    if (section==2) return [NSString stringWithFormat:@"QR-koppeling of lokaal noodnummer.\n\nYT Music IPS · %@\nYouTube Music %@", [NSBundle.mainBundle objectForInfoDictionaryKey:@"IPSBuildVersion"] ?: @"onbekend", [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"] ?: @"onbekend"];
    return section==0 ? [NSString stringWithFormat:@"%@ · Event %@. IPS gebruikt alleen audio en vraagt de hoogste aangeboden audiokwaliteit. Uitschakelen geeft de normale bediening terug.",IPSConnection,IPSEvent()] : section==1 ? @"SponsorBlock deelt het video-ID met sponsor.ajay.app en slaat alleen music_offtopic over. Normalisatie gebruikt de bestaande appfunctie; geen eigen AGC. Een hoge minimumkwaliteit kan nummers onbruikbaar maken als die stream ontbreekt. Auto blijft de veiligste keuze. Crossfade en fade-in/out kunnen niet tegelijk aan. Wijzigingen gelden voor volgende overgangen." : @"Scan de QR uit het beheer. Een lokaal audiobestand is nodig bij volledige internetuitval; de gecachte serverlijst heeft nog YouTube-verbinding nodig.";
}
- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)path {
    UITableViewCell *cell=[[UITableViewCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    if (path.section==0 && path.row<4) {
        UITextField *field=path.row==0 ? self.server : path.row==1 ? self.event : path.row==2 ? self.token : self.deviceName;
        [cell.contentView addSubview:field]; field.translatesAutoresizingMaskIntoConstraints=NO;
        [NSLayoutConstraint activateConstraints:@[[field.leadingAnchor constraintEqualToAnchor:cell.contentView.leadingAnchor constant:16],
            [field.trailingAnchor constraintEqualToAnchor:cell.contentView.trailingAnchor constant:-16],
            [field.topAnchor constraintEqualToAnchor:cell.contentView.topAnchor constant:8],
            [field.bottomAnchor constraintEqualToAnchor:cell.contentView.bottomAnchor constant:-8]]];
    } else if (path.section==1 && path.row>=6) {
        if(path.row==6){cell.textLabel.text=@"Crossfade";cell.accessoryView=self.queueCross;}
        else if(path.row==8){cell.textLabel.text=@"Fade-in / fade-out";cell.accessoryView=self.fade;}
        else {UITextField *field=path.row==7?self.crossSeconds:path.row==9?self.fadeInSeconds:self.fadeOutSeconds;
            cell.textLabel.text=path.row==7?@"Crossfade seconden (1-8)":path.row==9?@"Fade-in seconden (0-8)":@"Fade-out seconden (0-8)";
            cell.accessoryView=field;field.frame=CGRectMake(0,0,85,38);}
    } else if (path.section==1 && path.row==5) {
        cell.textLabel.text=@"Min. audiokwaliteit"; cell.accessoryView=self.minimumQuality;
    } else if (path.section==2) {
        cell.textLabel.text=path.row==0 ? @"Scan koppel-QR" : @"Kies lokaal noodnummer";
        cell.accessoryType=UITableViewCellAccessoryDisclosureIndicator;
    } else {
        cell.textLabel.text=path.section==0 ? @"IPS-modus actief" : @[@"Reclamefilter",@"Achtergrondaudio",@"SponsorBlock (optioneel)",@"Luidheidsnormalisatie",@"Scherm wakker houden"][path.row];
        cell.accessoryView=path.section==0 ? self.enabled : @[self.ads,self.background,self.sponsor,self.normalization,self.awake][path.row];
    }
    return cell;
}
- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)path {
    [tableView deselectRowAtIndexPath:path animated:YES];
    if (path.section!=2) return;
    if (path.row==0) IPSShowScanner(self);
    else IPSChooseEmergency(self);
}
- (void)cancel { [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)save {
    NSString *url=[self.server.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSURL *parsed=[NSURL URLWithString:url];
    NSString *event=[self.event.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    NSString *newToken=self.token.text ?: @"";
    NSString *error=nil;
    if(self.queueCross.on && self.fade.on)error=@"Kies crossfade of fade-in/out.";
    NSArray *fields=@[self.fadeInSeconds,self.fadeOutSeconds,self.crossSeconds]; NSMutableArray *seconds=NSMutableArray.array;
    for (NSUInteger i=0;i<fields.count;i++) {NSString *value=[((UITextField *)fields[i]).text stringByReplacingOccurrencesOfString:@"," withString:@"."];NSScanner *scanner=[NSScanner scannerWithString:value ?: @""];double number; if (![scanner scanDouble:&number] || !scanner.isAtEnd || !isfinite(number) || number<(i==2?1:0) || number>8) {error=@"Fade-in/out: 0 tot 8 sec; crossfade: 1 tot 8 sec.";break;}[seconds addObject:@(number)];}
    if (!error) {for(NSUInteger i=0;i<3;i++) [IPSD() setObject:seconds[i] forKey:@[@"IPS.fadeInSeconds",@"IPS.fadeOutSeconds",@"IPS.crossfadeSeconds"][i]];}
    if (self.enabled.on && (![parsed.scheme isEqual:@"https"] || !parsed.host.length || parsed.user || parsed.password || parsed.query || parsed.fragment)) error=@"Gebruik een HTTPS-server-URL zonder gebruikersgegevens, query of fragment.";
    if (event.length && !IPSMatch(event,@"^[A-Za-z0-9_-]{1,64}$")) error=@"Event-ID: maximaal 64 letters, cijfers, streepjes of underscores.";
    if (newToken.length && (newToken.length<24 || newToken.length>256)) error=@"De koppelsleutel moet 24 tot 256 tekens bevatten.";
    BOOL changed=![url isEqual:IPSSetting(@"url")] || ![event isEqual:IPSSetting(@"event")];
    if (self.enabled.on && !newToken.length && (changed || IPSToken().length<24)) error=@"Vul de koppelsleutel voor deze server en dit event in.";
    if (newToken.length && !error && !IPSStoreToken(newToken)) error=@"De koppelsleutel kon niet worden opgeslagen.";
    if (!error) [IPSD() setBool:self.queueCross.on forKey:@"IPS.queueCrossfade"];
    if (!error) [IPSD() setBool:NO forKey:@"IPS.nativeCrossfadeTest"];
    if (error) {
        UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Controleer instellingen" message:error preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil]; return;
    }
    [IPSD() setObject:[self.deviceName.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet] ?: @"" forKey:@"IPS.deviceName"]; [IPSD() setObject:url forKey:@"IPS.url"]; [IPSD() setObject:event forKey:@"IPS.event"];
    [IPSD() setBool:self.enabled.on forKey:@"IPS.enabled"];
    if (!self.enabled.on) IPSLeaveRemote();
    IPSFadeCancel(); [IPSD() setBool:self.fade.on forKey:@"IPS.fade"]; [IPSD() setBool:self.statusOverlay.on forKey:@"IPS.showStatusOverlay"];
    [IPSD() setInteger:self.minimumQuality.selectedSegmentIndex*10 forKey:@"IPS.minimumAudioQuality"];
    [IPSD() setBool:self.sponsor.on forKey:@"IPS.sponsorBlock"];
    [IPSD() setBool:self.normalization.on forKey:@"IPS.normalization"];
    [IPSD() setBool:self.awake.on forKey:@"IPS.keepAwake"];
    NSMutableDictionary *options=[[IPSD() dictionaryForKey:@"YTMUltimate"] mutableCopy] ?: NSMutableDictionary.dictionary;
    options[@"YTMUltimateIsEnabled"]=@YES; options[@"noAds"]=@(self.ads.on);
    options[@"backgroundPlayback"]=@(self.background.on); options[@"disableAutoRadio"]=@YES;
    [IPSD() setObject:options forKey:@"YTMUltimate"];
    if (changed) { IPSConfig=nil; IPSAllowFallback=NO; IPSOfflineFallback=NO; IPSFailures=0; IPSLastContact=nil; IPSDesiredID=nil; IPSEndedEvent=nil; IPSEndedID=nil; [IPSD() removeObjectForKey:@"IPS.desiredVideoId"]; }
    [IPSD() synchronize]; IPSGeneration++; IPSBusy=NO; IPSResetUserTransport(changed || !self.enabled.on); if(changed)IPSResetControlOrder(); IPSClockOffset=0;
    UIApplication.sharedApplication.idleTimerDisabled=self.enabled.on && self.awake.on;
    IPSNextPoll=0; [IPSEmergency stop]; IPSRefreshOverlay();
    [self dismissViewControllerAnimated:YES completion:^{ IPSTick(); }];
}
- (void)transitionChanged:(UISwitch *)sender {
    if(sender==self.queueCross && self.queueCross.on)[self.fade setOn:NO animated:YES];
    if(sender==self.fade && self.fade.on)[self.queueCross setOn:NO animated:YES];
    self.queueCross.enabled=!self.fade.on;self.fade.enabled=!self.queueCross.on;
    self.crossSeconds.enabled=self.queueCross.on;self.fadeInSeconds.enabled=self.fade.on;self.fadeOutSeconds.enabled=self.fade.on;
}

@end

@interface IPSMenu : NSObject
+ (void)show;
+ (void)gesture:(UILongPressGestureRecognizer *)gesture;
@end
@implementation IPSMenu
+ (void)show {
    UIViewController *presenter=IPSPresenter();
    if (!presenter || [presenter isKindOfClass:IPSSettingsController.class] || [presenter isKindOfClass:UIAlertController.class]) return;
    UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:[[IPSSettingsController alloc] initWithStyle:UITableViewStyleInsetGrouped]];
    nav.modalPresentationStyle=UIModalPresentationFormSheet;
    [presenter presentViewController:nav animated:YES completion:nil];
}
+ (void)gesture:(UILongPressGestureRecognizer *)gesture { if (gesture.state==UIGestureRecognizerStateBegan) [self show]; }
@end

@interface YTMAccountButton : UIButton
- (id)initWithTitle:(id)title identifier:(id)identifier icon:(id)icon actionBlock:(void(^)(BOOL))block;
@end
%hook YTMAvatarAccountView
- (void)setAccountMenuUpperButtons:(id)upper lowerButtons:(id)lower {
    id button=[[%c(YTMAccountButton) alloc] initWithTitle:@"IPS Player" identifier:@"ips-player" icon:[UIImage systemImageNamed:@"antenna.radiowaves.left.and.right"] actionBlock:^(BOOL finished){ [IPSMenu show]; }];
    NSMutableArray *buttons=[NSMutableArray arrayWithArray:lower ?: @[]];
    if (button) [buttons addObject:button];
    %orig(upper,buttons);
}
%end
%hook YTPlayerViewController
- (void)viewDidAppear:(BOOL)animated { %orig; if(IPSHoldActive() && self!=IPSPlayer){if(!(IPSCrossRunning() && self==IPSOutgoing && [IPSObject(self,@"currentVideoID") isEqual:IPSQueueOutgoing])){BOOL auxiliary=IPSAuxiliaryEvent;IPSAuxiliaryEvent=YES;IPSInvoke(self,@"pause");IPSAuxiliaryEvent=auxiliary;}return;} IPSPlayer=self; }
- (void)playbackController:(id)controller didActivateVideo:(id)video withPlaybackData:(id)data {
    if (IPSProbeConstructing || (self==IPSProbeSecondary && self!=IPSPlayer)) { if(IPSShouldMute(self)){float zero=0;IPSVolume(self,&zero,YES);} IPSAuxiliaryEvent=YES; %orig; IPSAuxiliaryEvent=NO; if(IPSShouldMute(self)){float zero=0;IPSVolume(self,&zero,YES);} return; } %orig; if(IPSHoldActive() && self!=IPSPlayer){if(!(IPSCrossRunning() && self==IPSOutgoing && [IPSObject(self,@"currentVideoID") isEqual:IPSQueueOutgoing])){BOOL auxiliary=IPSAuxiliaryEvent;IPSAuxiliaryEvent=YES;IPSInvoke(self,@"pause");IPSAuxiliaryEvent=auxiliary;}return;} IPSPlayer=self; IPSKnownPlaying=NO;
    IPSProgressAt=NSDate.date.timeIntervalSince1970; IPSProgressVideo=IPSVideo(); IPSLastPosition=0;
    if(!IPSCrossRunning() && [IPSVideo() isEqual:IPSDesiredID] && IPSPendingLoadCommand.length){IPSActiveLoadCommand=IPSPendingLoadCommand;IPSPendingLoadCommand=nil;}
    IPSAudioOnly(); IPSFetchSegments(); IPSFadeActivated();
    NSString *actual=IPSVideo();
    if (IPSEnabled() && IPSDesiredID.length && actual.length && ![actual isEqual:IPSDesiredID]) IPSInvoke(self,@"pause");
}
- (void)playbackControllerDidFinishPlayback:(id)controller {
    if (self==IPSProbeSecondary && self!=IPSPlayer) { %orig; return; }
    if (IPSDualHandleEnd(self)) { %orig; return; }
    NSString *finished=IPSVideo();
    %orig;
    if (IPSEnabled() && IPSDesiredID.length && [finished isEqual:IPSDesiredID]) {
        IPSEndedID=finished; IPSEndedEvent=NSUUID.UUID.UUIDString; IPSPlaying=NO; IPSKnownPlaying=YES;
        IPSInvoke(self,@"pause"); IPSNextPoll=0; IPSTick();
    }
}
%end
%hook YTMNowPlayingViewController
- (void)setPlaying:(BOOL)playing { if(IPSProbeConstructing || IPSAuxiliaryEvent){return;} BOOL changed=!IPSKnownPlaying || IPSPlaying!=playing; %orig; IPSNowPlaying=self; IPSPlaying=playing; IPSKnownPlaying=YES; if (changed) IPSNextPoll=0; }
%end

%ctor {
    dispatch_async(dispatch_get_main_queue(),^{
        [IPSD() setBool:NO forKey:@"IPS.nativeCrossfadeTest"];
        if([IPSD() boolForKey:@"IPS.queueCrossfade"])[IPSD() setBool:NO forKey:@"IPS.fade"];
        [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *notification){if(IPSHoldActive() && !IPSCrossRunning())IPSBindSurface();}];
        if (!IPSSetting(@"playerId").length) [IPSD() setObject:NSUUID.UUID.UUIDString forKey:@"IPS.playerId"];
        IPSDesiredID=IPSSetting(@"desiredVideoId");
        if (![IPSD() objectForKey:@"IPS.keepAwake"]) [IPSD() setBool:YES forKey:@"IPS.keepAwake"];
        NSMutableDictionary *options=[[IPSD() dictionaryForKey:@"YTMUltimate"] mutableCopy] ?: NSMutableDictionary.dictionary;
        for (NSString *key in @[@"YTMUltimateIsEnabled",@"noAds",@"backgroundPlayback",@"disableAutoRadio"]) if (!options[key]) options[key]=@YES;
        [IPSD() setObject:options forKey:@"YTMUltimate"];
        NSURLSessionConfiguration *config=NSURLSessionConfiguration.ephemeralSessionConfiguration;
        config.HTTPShouldSetCookies=NO; config.HTTPCookieStorage=nil; config.URLCache=nil;
        IPSSession=[NSURLSession sessionWithConfiguration:config delegate:IPSNetworkDelegate.new delegateQueue:nil];
        IPSConfig=[IPSD() dictionaryForKey:IPSConfigKey()];
        IPSAllowFallback=[IPSConfig[@"allowFallback"] boolValue];
        IPSFallbackIndex=[IPSConfig[@"nextIndex"] unsignedIntegerValue];
        IPSProgressAt=NSDate.date.timeIntervalSince1970;
        IPSStartPresentation();
        [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note){
            UIApplication.sharedApplication.idleTimerDisabled=IPSEnabled() && [IPSD() boolForKey:@"IPS.keepAwake"];
            for (UIWindow *window in UIApplication.sharedApplication.windows) if (window.isKeyWindow) {
                BOOL installed=NO;
                for (UIGestureRecognizer *g in window.gestureRecognizers) if (g.name && [g.name isEqual:@"IPS.settings"]) installed=YES;
                if (!installed) {
                    UILongPressGestureRecognizer *gesture=[[UILongPressGestureRecognizer alloc] initWithTarget:IPSMenu.class action:@selector(gesture:)];
                    gesture.name=@"IPS.settings"; gesture.numberOfTouchesRequired=3; gesture.minimumPressDuration=1;
                    gesture.cancelsTouchesInView=NO; [window addGestureRecognizer:gesture];
                }
            }
            IPSNextPoll=0; IPSTick(); IPSRefreshOverlay();
        }];
    });
}

#include "RemoteV6UI.h"
// Focused hooks based on YTMusicUltimate e5530b1 and verified 8.21.3 method metadata.
// GPL-3.0 provenance: ../native-login-upstream/LICENSE and V6-PROVENANCE.md.
%hook UIView
- (void)didAddSubview:(UIView *)view { %orig; IPSMaintainRemoteSlot(self); }
%end
%hook YTMVideoOverlayView
- (void)layoutSubviews { %orig; IPSLayoutRemoteAction((UIView *)self); }
%end
%hook YTMActionRowView
- (void)layoutSubviews { %orig; IPSLayoutRemoteAction((UIView *)self); }
- (void)setButtonRenderers:(id)renderers { %orig; IPSQueueActionRefresh((UIView *)self); }
%end
%hook YTMNowPlayingView
- (void)layoutSubviews { %orig; IPSLayoutRemoteAction((UIView *)self); }
- (void)setActionBarWithElementRenderer:(id)renderer parentResponder:(id)responder context:(id)context { %orig; IPSQueueActionRefresh((UIView *)self); }
- (void)setSecondaryActionBarWithElementRenderer:(id)renderer parentResponder:(id)responder context:(id)context { %orig; IPSQueueActionRefresh((UIView *)self); }
- (void)setActionBarWithButtonRenderers:(id)renderers delegate:(id)delegate { %orig; IPSQueueActionRefresh((UIView *)self); }
%end
%hook YTMSettingsImpl
- (BOOL)initialFormatAudioOnly { return IPSEnabled() ? YES : %orig; }
- (BOOL)isAlbumVolumeNormalizationEnabled { return IPSEnabled() && [IPSD() boolForKey:@"IPS.normalization"] ? YES : %orig; }
- (id)initialFormatAudioQualityConfig {
    id original=%orig;
    if (!IPSEnabled()) return original;
    // Original 8.21.3 initializer uses int qualities 10/20/30; 30 is the high tier.
    // Keep minimum zero: a missing high stream must not make a track unplayable.
    Class cls=NSClassFromString(@"MLAudioQualityConfig");
    SEL sel=NSSelectorFromString(@"initWithMinWiFiQuality:maxWiFiQuality:minCellularQuality:maxCellularQuality:");
    id allocated=[cls alloc]; NSMethodSignature *sig=[allocated methodSignatureForSelector:sel];
    if (!sig || sig.numberOfArguments!=6 || sig.methodReturnType[0]!='@') return original;
    for (NSUInteger i=2;i<6;i++) if (strcmp([sig getArgumentTypeAtIndex:i],@encode(int))) return original;
    int minimum=(int)[IPSD() integerForKey:@"IPS.minimumAudioQuality"];
    if (minimum!=10 && minimum!=20 && minimum!=30) minimum=0;
    id result=((id(*)(id,SEL,int,int,int,int))objc_msgSend)(allocated,sel,minimum,30,minimum,30);
    return result ?: original;
}
%end
%hook YTMQueueConfigImpl
- (BOOL)noVideoModeEnabledForMusic { return IPSEnabled() ? YES : %orig; }
- (BOOL)noVideoModeEnabledForPodcasts { return IPSEnabled() ? YES : %orig; }
%end
%hook YTMAudioVideoModeControllerInternalImpl
- (BOOL)isAudioOnlyBlocked { return IPSEnabled() ? NO : %orig; }
%end
%hook YTIPlayerResponse
- (BOOL)ytm_isAudioOnlyPlayable { return IPSEnabled() ? YES : %orig; }
- (BOOL)isAudioOnlyAvailabilityBlocked { return IPSEnabled() ? NO : %orig; }
- (id)ytm_audioOnlyUpsell { return IPSEnabled() ? nil : %orig; }
%end
%hook YTUserDefaults
- (BOOL)enableAlbumVolumeNormalization { return IPSEnabled() && [IPSD() boolForKey:@"IPS.normalization"] ? YES : %orig; }
%end
%hook YTYouThereControllerImpl
- (BOOL)shouldShowYouTherePrompt { return IPSEnabled() ? NO : %orig; }
- (void)showYouTherePrompt { if (!IPSEnabled()) %orig; }
%end
%hook YTColdConfig
- (BOOL)enableYouthereCommandsOnIos { return IPSEnabled() ? NO : %orig; }
- (BOOL)iosEnableNewPromoForcingSettingsPage { return IPSEnabled() ? NO : %orig; }
%end
%hook YTMUpsellDialogController
- (void)showUpsellDialogWithUpsell:(id)upsell upsellParentResponder:(id)responder { if (!IPSEnabled()) %orig; }
- (void)showUpsellDialogWithUpsellResponderEvent:(id)event { if (!IPSEnabled()) %orig; }
- (void)showUpsellDialogWithUpsell:(id)upsell videoID:(id)video toastType:(long long)type upsellParentResponder:(id)responder shouldDismissOnBackgroundTap:(BOOL)dismiss { if (!IPSEnabled()) %orig; }
%end
%hook YTMIntegrationsSettingsViewController
- (void)showUpsellingDialog { if (!IPSEnabled()) %orig; }
- (void)showPromotionScreen { if (!IPSEnabled()) %orig; }
%end
%hook YTMAppResponderImpl
- (void)presentFullscreenPromoForEvent:(id)event { if (!IPSEnabled()) %orig; }
- (void)presentInterstitialGridPromoForEvent:(id)event { if (!IPSEnabled()) %orig; }
- (void)presentInterstitialPromoForEvent:(id)event { if (!IPSEnabled()) %orig; }
%end
%hook YTPromosheetController
- (void)presentPromosheetWithEvent:(id)event { if (!IPSEnabled()) %orig; }
%end
static BOOL IPSAccountToast(id message) {
    // Only a recognizable account confirmation during an IPS track load.
    // Never hide sign-in failures, alerts or general playback errors.
    if (!IPSEnabled() || !IPSLastRemoteLoad || NSDate.date.timeIntervalSince1970-IPSLastRemoteLoad>10) return NO;
    NSString *text=[message isKindOfClass:NSString.class] ? message : [message isKindOfClass:NSAttributedString.class] ? [message string] : nil;
    if (!text) return NO;
    return IPSMatch(text,@"(?i)^(?:je bent (?:nu )?ingelogd als|ingelogd als|signed in as|you(?:â€™|')?re signed in as)\\s+.+");
}
%hook YTMToastController
- (void)showMessage:(id)message { if (!IPSAccountToast(message)) %orig; }
- (void)showMessage:(id)message infoType:(long long)type { if (!IPSAccountToast(message)) %orig; }
- (void)showMessage:(id)message infoType:(long long)type duration:(double)duration { if (!IPSAccountToast(message)) %orig; }
- (void)showMessage:(id)message action:(id)action infoType:(long long)type duration:(double)duration { if (!IPSAccountToast(message)) %orig; }
- (void)showMessage:(id)message HUDMessageAction:(id)action infoType:(long long)type duration:(double)duration { if (!IPSAccountToast(message)) %orig; }
%end

%hook YTPlayerViewController
- (id)initWithServiceRegistryScope:(id)scope parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator {
    id result=%orig;IPSProbeRememberFactory(scope,parent,overlay,coordinator);return result;
}
- (id)initWithServiceRegistryScope:(id)scope parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator shouldDisableInternalPlayabilityResolution:(BOOL)a shouldDisableSpeedyGNotifications:(BOOL)b shouldDelayAdsPlaybackCoordinatorCreation:(BOOL)c shouldDisablePromoNotifications:(BOOL)d {
    id result=%orig;IPSProbeRememberFactory(scope,parent,overlay,coordinator);return result;
}
- (id)initWithServiceRegistryScope:(id)scope parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator shouldDisableInternalPlayabilityResolution:(BOOL)a shouldDisableSpeedyGNotifications:(BOOL)b shouldDelayAdsPlaybackCoordinatorCreation:(BOOL)c shouldDisablePromoNotifications:(BOOL)d shouldEnableMediaDrivenPrefetch:(BOOL)e {
    id result=%orig;IPSProbeRememberFactory(scope,parent,overlay,coordinator);return result;
}
- (void)loadWithPlayerPlayback:(id)playback { IPSProbeRememberPlayback(self,playback);%orig; }
%end

%hook YTPlayerViewController
- (id)initWithServiceRegistryScope:(id)scope playerViewControllerTraits:(id)traits parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator {
    id result=%orig;IPSProbeRememberFactory9(scope,traits,parent,overlay,coordinator);return result;
}
- (id)initWithServiceRegistryScope:(id)scope playerViewControllerTraits:(id)traits parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator recompositeProvider:(id)recomposite shouldDisableInternalPlayabilityResolution:(BOOL)a shouldDelayAdsPlaybackCoordinatorCreation:(BOOL)b shouldDisablePromoNotifications:(BOOL)c shouldEnableMediaDrivenPrefetch:(BOOL)d {
    id result=%orig;IPSProbeRememberFactory9(scope,traits,parent,overlay,coordinator);return result;
}
- (id)initWithServiceRegistryScope:(id)scope playerViewControllerTraits:(id)traits parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator recompositeProvider:(id)recomposite playabilityResolutionUIController:(id)resolution shouldDelayAdsPlaybackCoordinatorCreation:(BOOL)b shouldDisablePromoNotifications:(BOOL)c shouldEnableMediaDrivenPrefetch:(BOOL)d {
    id result=%orig;IPSProbeRememberFactory9(scope,traits,parent,overlay,coordinator);return result;
}
%end

%hook YTPlayerViewController
- (void)setVolume:(float)volume { %orig(IPSShouldMute(self) ? 0 : volume); }
- (float)volume { return IPSShouldMute(self) ? 0 : %orig; }
- (void)play { if(IPSLocalPlaybackPaused())return; if(IPSHoldActive() && self!=IPSPlayer && self!=IPSProbeSecondary)return; if (IPSShouldMute(self)) {float zero=0;IPSVolume(self,&zero,YES);} %orig; }
- (void)playbackController:(id)controller didLoadContentPlaybackData:(id)data {
    if (IPSShouldMute(self)) {float zero=0;IPSVolume(self,&zero,YES);} %orig;
    if (IPSShouldMute(self)) {float zero=0;IPSVolume(self,&zero,YES);}
}
%end

%hook YTPlayerViewController
- (void)seekToTime:(double)t { id target=IPSSeekTarget(self); if(target){((void(*)(id,SEL,double))objc_msgSend)(target,_cmd,t);return;} %orig; }
- (void)seekToTime:(double)t seekSource:(int)source { id target=IPSSeekTarget(self); if(target){((void(*)(id,SEL,double,int))objc_msgSend)(target,_cmd,t,source);return;} %orig; }
- (void)seekToTime:(double)t toleranceBefore:(double)b toleranceAfter:(double)a seekSource:(int)source { id target=IPSSeekTarget(self); if(target){((void(*)(id,SEL,double,double,double,int))objc_msgSend)(target,_cmd,t,b,a,source);return;} %orig; }
%end
// A temporary outgoing player must not erase the main player's system metadata.
%hook MPNowPlayingInfoCenter
- (void)setNowPlayingInfo:(NSDictionary *)info {
    if(IPSPublishingSystemInfo){%orig;return;}
    if(IPSHoldActive() && !IPSInactive)return;
    if(IPSProbeConstructing || IPSAuxiliaryEvent || (IPSEnabled() && IPSCrossRunning() && !info.count))return;
    %orig;
}
%end

// The app also has an end path that internally transitions before DidFinish.
%hook YTPlayerViewController
- (void)playbackController:(id)controller didFinishPlaybackAndWillInternallyTransitionToNextPlayback:(BOOL)next {
    BOOL outgoing=IPSCrossRunning() && self==IPSOutgoing;
    if(outgoing)IPSOutgoingEnded=YES;
    else if(IPSEnabled() && self==IPSPlayer && [IPSVideo() isEqual:IPSDesiredID])IPSMarkEnded(IPSVideo());
    %orig;
    if(!outgoing)IPSNextPoll=0;
}
%end

%hook YTMWatchViewController
- (void)setPlayerViewController:(id)player { if(IPSHoldActive() && self==IPSWatchHost && !IPSBindingSurface && player!=IPSPlayer)return; %orig; IPSRememberWatch(self,player); }
- (id)detachPlayerViewController { if(IPSHoldActive() && self==IPSWatchHost && !IPSBindingSurface && IPSObject(self,@"playerViewController")==IPSPlayer)return IPSPlayer;return %orig; }
- (BOOL)isMatchingVideoOnPlayerAndWatchNext { if(IPSHoldActive() && self==IPSWatchHost)return YES;return %orig; }
- (void)setVideoTitle:(id)title videoArtist:(id)artist {
    if(IPSHoldActive() && self==IPSWatchHost){
        @try{
            NSDictionary *track=IPSTrackPresentation();id nativeTitle=IPSNativeTitleText(track[@"title"]),nativeArtist=IPSNativeFormattedText(track[@"artist"]);
            if(nativeTitle && nativeArtist)%orig(nativeTitle,nativeArtist);
            else IPSCrossInfo=@"Native titelweergave niet beschikbaar; IPS-metadata blijft actief.";
        }@catch(NSException *error){IPSCrossInfo=[@"Titelweergave overgeslagen: " stringByAppendingString:error.reason ?: @"onbekend"];}
        return;
    }%orig;
}
- (void)updateForPlayerState:(NSInteger)state { %orig(self==IPSWatchHost?IPSPresentedPlayerState(state):state); }
- (void)updateWatchViewForPlayerState:(NSInteger)state { %orig(self==IPSWatchHost?IPSPresentedPlayerState(state):state); }
- (void)updateNowPlayingViewControllerForPlayerState:(NSInteger)state { %orig(self==IPSWatchHost?IPSPresentedPlayerState(state):state); }
- (void)updateMiniPlayerViewControllerForPlayerState:(NSInteger)state { %orig(self==IPSWatchHost?IPSPresentedPlayerState(state):state); }
%end
%hook YTLocalPlaybackCoordinator
- (BOOL)isAnyControllerActivelyPlaying {
    BOOL original=%orig;if(original)return YES;
    static BOOL checking=NO;
    if(checking || !IPSHoldActive() || IPSInactive || self!=IPSProbeFactory.lastObject || self==IPSOwnedCoordinator || self==IPSQueueCoordinator)return NO;
    BOOL playing=NO;checking=YES;
    @try{IPSActualPlaying(&playing);}@finally{checking=NO;}
    return playing;
}
%end
%hook YTMAppDelegate
- (BOOL)shouldAudioSessionActivateInBackground { if(IPSHoldActive() && [[[IPSD() dictionaryForKey:@"YTMUltimate"] objectForKey:@"backgroundPlayback"] boolValue])return YES;return %orig; }
%end
%hook YTMPlayerCarouselCell
- (void)updateThumbnail { if(IPSUpdateArtwork(self))return;objc_setAssociatedObject(self,&IPSArtworkVideoKey,nil,OBJC_ASSOCIATION_COPY_NONATOMIC);%orig;IPSUpdateArtwork(self); }
- (void)prepareForReuse { objc_setAssociatedObject(self,&IPSArtworkVideoKey,nil,OBJC_ASSOCIATION_COPY_NONATOMIC);%orig; }
%end
%hook YTMNowPlayingViewController
- (void)didTapPlayButton { if(IPSEnabled() && IPSPlayer){IPSRequestToggle();return;} %orig; }
- (void)didTapNextButton { if(IPSEnabled() && IPSPlayer){IPSRequestTransport(@"next");return;} %orig; }
- (void)didTapPrevButton { if(IPSEnabled() && IPSPlayer)return;%orig; }
%end
%hook YTMRemoteCommandHandler
- (NSInteger)handlePlayCommand:(id)event { if(IPSEnabled() && IPSPlayer){IPSRequestTransport(@"resume");return 0;} return %orig; }
- (NSInteger)handlePauseCommand:(id)event { if(IPSEnabled() && IPSPlayer){IPSRequestTransport(@"pause");return 0;} return %orig; }
- (NSInteger)handleTogglePlayPauseCommand:(id)event { if(IPSEnabled() && IPSPlayer){IPSRequestToggle();return 0;} return %orig; }
- (void)handleRemoteControlEvent:(NSInteger)event {
    if(IPSEnabled() && IPSPlayer){
        if(event==UIEventSubtypeRemoteControlPlay)IPSRequestTransport(@"resume");
        else if(event==UIEventSubtypeRemoteControlPause || event==UIEventSubtypeRemoteControlStop)IPSRequestTransport(@"pause");
        else if(event==UIEventSubtypeRemoteControlTogglePlayPause)IPSRequestToggle();
        else if(event==UIEventSubtypeRemoteControlNextTrack)IPSRequestTransport(@"next");
        return;
    }%orig;
}
%end

%hook YTImageView
- (void)setImage:(id)image animated:(BOOL)animated { if(IPSHoldActive() && [objc_getAssociatedObject(self,&IPSActualCoverKey) boolValue]){%orig(IPSPresentedImage,NO);return;}%orig; }
%end
%hook YTMContentViewController
- (void)presentPromosheetWithEvent:(id)event { if(!IPSEnabled())%orig; }
%end
%hook YTMOverlayController
- (void)presentUpsellDialog:(id)dialog { if(!IPSEnabled())%orig; }
%end
%hook YTMAppDelegate
- (void)showUpsellAlertWithTitle:(id)title subtitle:(id)subtitle upgradeButtonTitle:(id)button upsellURLString:(id)url sourceApplication:(id)source { if(!IPSEnabled())%orig; }
%end
