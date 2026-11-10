// Included in RemoteV6.x: all state and player calls stay on the main queue.
static NSString *IPSConfigKey(void) { return [NSString stringWithFormat:@"IPS.config.%@.%@",IPSSetting(@"url"),IPSEvent()]; }
static NSArray *IPSSegments;
static NSString *IPSSegmentVideo;
static NSMutableSet *IPSSkipped;
static NSTimeInterval IPSEmergencyAttemptAt;
// Initial-format hooks select audio for new loads. Do not switch the live
// controller's audio/video pipeline while seeking or resolving another track.
static void IPSAudioOnly(void) {}
static void IPSLeaveRemote(void) {
    IPSReleasePresentation();
    NSString *resumeUI=IPSOwnedActive?IPSVideo():nil;
    IPSQueueCancel(NO); IPSProbeStop(); IPSFadeCancel();
    IPSDesiredID=nil;IPSEndedID=nil;IPSEndedEvent=nil;IPSLoadAt=0;
    IPSAllowFallback=NO;IPSOfflineFallback=NO;IPSSegments=nil;
    [IPSD() removeObjectForKey:@"IPS.desiredVideoId"];
    IPSDualReturnToUI();
    SEL reset=NSSelectorFromString(@"setUserAudioOnlyModeActive:");
    NSMethodSignature *sig=[IPSPlayer methodSignatureForSelector:reset];
    if(sig && sig.numberOfArguments==3 && !strcmp(sig.methodReturnType,@encode(void)) && !strcmp([sig getArgumentTypeAtIndex:2],@encode(BOOL)))
        ((void(*)(id,SEL,BOOL))objc_msgSend)(IPSPlayer,reset,NO);
    if(resumeUI.length)IPSLoad(resumeUI);
    SEL active=NSSelectorFromString(@"setActivePlayback:");
    if([IPSPlayer respondsToSelector:active])((void(*)(id,SEL,BOOL))objc_msgSend)(IPSPlayer,active,YES);
}
static BOOL IPSSeek(double time) {
    SEL sel=NSSelectorFromString(@"seekToTime:");
    NSMethodSignature *sig=[IPSPlayer methodSignatureForSelector:sel];
    if (!sig || sig.numberOfArguments!=3 || strcmp(sig.methodReturnType,@encode(void))) return NO;
    const char *type=[sig getArgumentTypeAtIndex:2];
    if (!strcmp(type,@encode(double))) ((void(*)(id,SEL,double))objc_msgSend)(IPSPlayer,sel,time);
    else if (!strcmp(type,@encode(float))) ((void(*)(id,SEL,float))objc_msgSend)(IPSPlayer,sel,(float)time);
    else return NO;
    return YES;
}
static void IPSFetchSegments(void) {
    IPSSegments=nil; IPSSkipped=NSMutableSet.set;
    if (!IPSEnabled() || ![IPSD() boolForKey:@"IPS.sponsorBlock"]) return;
    NSString *video=IPSVideo();
    if (!IPSMatch(video,@"^[A-Za-z0-9_-]{11}$")) return;
    IPSSegmentVideo=video;
    NSUInteger generation=IPSGeneration;
    NSURL *url=[NSURL URLWithString:[NSString stringWithFormat:@"https://sponsor.ajay.app/api/skipSegments?videoID=%@&categories=%%5B%%22music_offtopic%%22%%5D",video]];
    NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:url]; request.timeoutInterval=5;
    // Separate request: no IPS bearer token, player-ID or account cookies.
    [[IPSSession dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
        if (error || [(NSHTTPURLResponse *)response statusCode]!=200 || data.length>131072) return;
        id parsed=[NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
        if (![parsed isKindOfClass:NSArray.class]) return;
        NSMutableArray *valid=NSMutableArray.array;
        for (id item in parsed) {
            if (![item isKindOfClass:NSDictionary.class] || ![item[@"category"] isEqual:@"music_offtopic"]) continue;
            NSArray *range=item[@"segment"];
            if (![range isKindOfClass:NSArray.class] || range.count!=2 || ![range[0] isKindOfClass:NSNumber.class] || ![range[1] isKindOfClass:NSNumber.class]) continue;
            double start=[range[0] doubleValue], end=[range[1] doubleValue];
            if (!isfinite(start)||!isfinite(end)||start<0||end<=start||end>86400) continue;
            [valid addObject:@[@(start),@(end)]];
            if (valid.count>=100) break;
        }
        dispatch_async(dispatch_get_main_queue(),^{ if (generation==IPSGeneration && [video isEqual:IPSVideo()]) IPSSegments=valid; });
    }] resume];
}
static void IPSAfterCommand(NSString *action) {
    if (![action isEqual:@"load"]) IPSQueueCancel(YES);
    if (![action isEqual:@"load"]) IPSFadeCancel();
    IPSOfflineFallback=NO; [IPSEmergency stop];
    IPSAllowFallback=![action isEqual:@"pause"] && ![action isEqual:@"stop"];
    IPSFault=nil; IPSFaultID=nil; IPSFaultAt=NSDate.date.timeIntervalSince1970;
    IPSProgressAt=IPSFaultAt;
    NSMutableDictionary *cache=[IPSConfig mutableCopy];
    if (cache) { cache[@"allowFallback"]=@(IPSAllowFallback); [IPSD() setObject:cache forKey:IPSConfigKey()]; }
}
static void IPSStartEmergency(void) {
    if (IPSEmergency.playing || !IPSAllowFallback || NSDate.date.timeIntervalSince1970-IPSEmergencyAttemptAt<60) return;
    IPSEmergencyAttemptAt=NSDate.date.timeIntervalSince1970;
    NSString *path=[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:@"IPS/emergency.audio"];
    IPSEmergency=[[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:path] error:NULL];
    if (IPSEmergency) {
        IPSInvoke(IPSPlayer,@"pause");
        [AVAudioSession.sharedInstance setCategory:AVAudioSessionCategoryPlayback error:NULL];
        [AVAudioSession.sharedInstance setActive:YES error:NULL];
        IPSEmergency.numberOfLoops=-1;
        [IPSEmergency play]; IPSOfflineFallback=YES;
    }
}
static void IPSStartFallback(void) {
    if (!IPSAllowFallback || ![IPSConfig[@"online"] boolValue] || IPSEmergency.playing) return;
    NSArray *tracks=IPSConfig[@"tracks"];
    if (![tracks isKindOfClass:NSArray.class] || !tracks.count) { IPSStartEmergency(); return; }
    NSDictionary *track=tracks[IPSFallbackIndex%tracks.count];
    NSString *video=track[@"id"];
    if (!IPSMatch(video,@"^[A-Za-z0-9_-]{11}$")) return;
    IPSFallbackIndex=(IPSFallbackIndex+1)%tracks.count;
    NSMutableDictionary *cache=[IPSConfig mutableCopy]; cache[@"nextIndex"]=@(IPSFallbackIndex);
    IPSConfig=cache; [IPSD() setObject:cache forKey:IPSConfigKey()];
    IPSDesiredID=video; IPSLoadAt=NSDate.date.timeIntervalSince1970; IPSProgressAt=IPSLoadAt;
    IPSEndedEvent=nil; IPSEndedID=nil; IPSKnownPlaying=NO;
    IPSOfflineFallback=YES;
    if (!IPSLoad(video)) IPSStartEmergency();
}
#include "IPSPlayerControls.h"
static void IPSTick(void) {
    NSTimeInterval now=NSDate.date.timeIntervalSince1970;
    if (IPSBusy || !IPSEnabled() || now<IPSNextPoll) return;
    if(IPSSendTransportIfPending())return;
    IPSBusy=YES;
    NSMutableDictionary *status=[IPSStatus() mutableCopy];
    status[@"deviceName"]=IPSSetting(@"deviceName").length ? IPSSetting(@"deviceName") : UIDevice.currentDevice.name;
    status[@"deviceType"]=UIDevice.currentDevice.userInterfaceIdiom==UIUserInterfaceIdiomPad ? @"tablet" : @"phone";
    status[@"configVersion"]=IPSConfig[@"version"] ?: @"";
    BOOL usableFallback=IPSOfflineFallback && [status[@"videoId"] isEqual:IPSDesiredID] && [@[@"playing",@"paused",@"ended"] containsObject:status[@"playbackState"]];
    status[@"offlineFallback"]=@(usableFallback && !IPSEmergency.playing);
    status[@"fallbackPending"]=@(IPSOfflineFallback && !usableFallback && !IPSEmergency.playing);
    status[@"localEmergency"]=@(IPSEmergency.playing);
    for (NSDictionary *track in IPSConfig[@"tracks"]) if ([track[@"id"] isEqual:IPSVideo()]) { status[@"fallbackTitle"]=track[@"title"] ?: @""; break; }
    status[@"fault"]=IPSFault ?: @""; status[@"faultId"]=IPSFaultID ?: @"";
    if (IPSEmergency.playing || (IPSOfflineFallback && !usableFallback)) {
        status[@"videoId"]=@""; status[@"playbackState"]=@"unknown";
        status[@"endedEvent"]=@""; status[@"endedVideoId"]=@"";
    }
    IPSRequest(@"sync",status,^(NSDictionary *reply) {
        double time=NSDate.date.timeIntervalSince1970;
        if (!reply) {
            IPSFailures=MIN(IPSFailures+1,12);
            // At most one retry per minute, plus small jitter to avoid bursts.
            double delay=MIN(60.0,4.0*pow(2,MIN(IPSFailures-1,4)));
            IPSNextPoll=time+delay+(arc4random_uniform(1000)/1000.0);
            if (IPSHTTPStatus==401 || IPSHTTPStatus==403 || IPSHTTPStatus==409 || IPSHTTPStatus==404) IPSAllowFallback=NO;
            IPSBusy=NO; IPSRefreshOverlay(); return;
        }
        IPSInactive=[reply[@"activePlayer"] respondsToSelector:@selector(boolValue)] && ![reply[@"activePlayer"] boolValue];
        IPSLastContact=NSDate.date; IPSFailures=0; IPSNextPoll=time+4;
        if (IPSInactive) { IPSConnection=@"Server bereikbaar - ander device actief"; IPSFadeCancel(); IPSAllowFallback=NO; NSMutableDictionary *inactiveCache=[IPSConfig mutableCopy]; if (inactiveCache) {inactiveCache[@"allowFallback"]=@NO; [IPSD() setObject:inactiveCache forKey:IPSConfigKey()];} IPSInvoke(IPSPlayer,@"pause"); [IPSEmergency stop]; IPSBusy=NO; IPSRefreshOverlay(); return; }
        NSDictionary *config=reply[@"config"];
        if ([config isKindOfClass:NSDictionary.class]) {
            NSMutableDictionary *cache=[config mutableCopy];
            if (!cache[@"tracks"] && [cache[@"version"] isEqual:IPSConfig[@"version"]]) cache[@"tracks"]=IPSConfig[@"tracks"] ?: @[];
            if (![cache[@"tracks"] isKindOfClass:NSArray.class]) cache[@"tracks"]=@[];
            // An administrative pause / live show must survive an outage/relaunch.
            IPSAllowFallback=[cache[@"online"] boolValue] && [cache[@"automatic"] boolValue] && !IPSLocalUserPaused;
            cache[@"allowFallback"]=@(IPSAllowFallback);
            IPSConfig=cache; IPSFallbackIndex=[cache[@"nextIndex"] unsignedIntegerValue];
            if (![[IPSD() dictionaryForKey:IPSConfigKey()] isEqual:cache]) [IPSD() setObject:cache forKey:IPSConfigKey()];
        }
        if (IPSOfflineFallback && [reply[@"reconnectAccepted"] boolValue]) {
            IPSOfflineFallback=NO;
            IPSEndedEvent=nil; IPSEndedID=nil;
            // Keep streamed fallback playing until it ends; server adopts its ID.
            // Local emergency audio ends only when a new load/resume arrives.
        }
        if (![IPSConfig[@"online"] boolValue]) { IPSInvoke(IPSPlayer,@"pause"); [IPSEmergency stop]; }
        IPSRefreshOverlay();
        IPSCommand(reply[@"command"]);
    });
}
static void IPSMonitor(void) {
    if (!IPSEnabled()) return;
    NSTimeInterval now=NSDate.date.timeIntervalSince1970;
    NSDictionary *status=IPSStatus(); IPSFadeUpdate(status);
    NSString *video=status[@"videoId"], *state=status[@"playbackState"];
    BOOL finished=NO;if(!IPSCrossRunning() && IPSBool(IPSPlayer,@"isPlaybackFinished",&finished) && finished && [video isEqual:IPSDesiredID] && (!IPSEndedEvent || ![IPSEndedID isEqual:video])){IPSMarkEnded(video);state=@"ended";}
    id raw=status[@"position"]; double position=[raw isKindOfClass:NSNumber.class] ? [raw doubleValue] : 0;
    if (![video isEqual:IPSProgressVideo] || position>IPSLastPosition+0.25 || position<IPSLastPosition-2) {
        IPSProgressVideo=video; IPSProgressAt=now; IPSLastPosition=position; IPSFault=nil; IPSFaultID=nil;
    }
    if ([state isEqual:@"playing"] && [video isEqual:IPSDesiredID]) IPSLoadAt=0;
    BOOL noServer=IPSFailures>0 && (!IPSLastContact || -IPSLastContact.timeIntervalSinceNow>18);
    if (noServer && IPSAllowFallback) {
        if (IPSEndedEvent || (!video.length && !IPSLoadAt)) IPSStartFallback();
        else if ((IPSLoadAt && now-IPSLoadAt>25) || ([state isEqual:@"playing"] && now-IPSProgressAt>35)) IPSStartEmergency();
    }
    // Poll a little sooner only near the end, leaving preparation time for B.
    id observedDuration=status[@"duration"];double remaining=[observedDuration isKindOfClass:NSNumber.class]?[observedDuration doubleValue]-position:INFINITY;
    if(!noServer && IPSQueueReady() && [state isEqual:@"playing"] && remaining>0 && remaining<=14 && (!IPSLastContact || -IPSLastContact.timeIntervalSinceNow>=1.5)){IPSNextPoll=0;IPSTick();}
    // Faults are observations only: recovery decisions belong to the server.
    NSString *fault=nil;
    if (!noServer && IPSAllowFallback) {
        if (IPSLoadAt && now-IPSLoadAt>20) fault=@"not-started";
        else if ([state isEqual:@"playing"] && now-IPSProgressAt>30) fault=@"stalled";
    }
    if (fault && (!IPSFaultID || now-IPSFaultAt>40)) {
        IPSFault=fault; IPSFaultID=NSUUID.UUID.UUIDString; IPSFaultAt=now; IPSNextPoll=0;
    }
    if ([IPSD() boolForKey:@"IPS.sponsorBlock"] && [state isEqual:@"playing"] && [video isEqual:IPSSegmentVideo]) {
        for (NSUInteger i=0;i<IPSSegments.count;i++) {
            NSArray *range=IPSSegments[i];
            double start=[range[0] doubleValue], end=[range[1] doubleValue];
            id duration=status[@"duration"];
            if (![duration isKindOfClass:NSNumber.class] || end>=[duration doubleValue]) continue;
            if (position>=start && position<end-0.5 && ![IPSSkipped containsObject:@(i)] && IPSSeek(end)) { [IPSSkipped addObject:@(i)]; break; }
        }
    }
}
