// One presentation owner: the audible IPS player, independent of the old queue.
static NSMutableDictionary *IPSMetadata;
static NSString *IPSPresentedVideo,*IPSPresentedTitle,*IPSPresentedArtist;
static UIImage *IPSPresentedImage;
static char IPSActualCoverKey;
static void IPSRefreshActualCovers(UIView *view){
    if(!view)return;
    Class overlay=NSClassFromString(@"YTMVideoOverlayView");
    if(overlay && [view isKindOfClass:overlay]){
        id image=IPSObject(view,@"thumbnailView");SEL setter=NSSelectorFromString(@"setImage:animated:");
        if([image respondsToSelector:setter]){objc_setAssociatedObject(image,&IPSActualCoverKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);((void(*)(id,SEL,id,BOOL))objc_msgSend)(image,setter,IPSPresentedImage,NO);}
    }
    for(UIView *child in view.subviews)IPSRefreshActualCovers(child);
}
static NSURLSession *IPSArtworkSession;
static NSURLSessionDataTask *IPSArtworkTask;
static NSCache *IPSArtworkCache;
static NSTimeInterval IPSLastPublish,IPSLastSurfaceRepair;
static BOOL IPSLastPublishedPlaying,IPSBackgroundAudioReady;
static dispatch_source_t IPSPresentationClock;
static UIBackgroundTaskIdentifier IPSExitTask;
static NSDictionary *IPSCurrentSystemInfo;
static NSMutableArray *IPSSystemCommandTargets;
static void IPSBindSystemCommands(void){
    if(IPSSystemCommandTargets.count)return;
    IPSSystemCommandTargets=NSMutableArray.new;
    MPRemoteCommandCenter *center=MPRemoteCommandCenter.sharedCommandCenter;
    for(NSArray *pair in @[@[center.playCommand,@"resume"],@[center.pauseCommand,@"pause"],@[center.togglePlayPauseCommand,@"toggle"],@[center.nextTrackCommand,@"next"]]){
        MPRemoteCommand *command=pair[0];NSString *action=pair[1];command.enabled=YES;
        id token=[command addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent *event){
            dispatch_async(dispatch_get_main_queue(),^{if(!IPSHoldActive() || IPSInactive)return;if([action isEqual:@"toggle"])IPSRequestToggle();else IPSRequestTransport(action);});
            return MPRemoteCommandHandlerStatusSuccess;
        }];
        if(token)[IPSSystemCommandTargets addObject:@[command,token]];
    }
}

static NSString *IPSString(id value){return [value isKindOfClass:NSString.class]?value:@"";}
static void IPSRememberMetadata(NSDictionary *command){
    NSDictionary *metadata=command[@"metadata"];NSString *video=IPSString(command[@"videoId"]);
    if(![metadata isKindOfClass:NSDictionary.class] || ![metadata[@"videoId"] isEqual:video] || !IPSMatch(video,@"^[A-Za-z0-9_-]{11}$"))return;
    if(!IPSMetadata)IPSMetadata=NSMutableDictionary.new;
    if(IPSMetadata.count>12)[IPSMetadata removeAllObjects];
    IPSMetadata[video]=@{@"title":[IPSString(metadata[@"title"]) substringToIndex:MIN(512,IPSString(metadata[@"title"]).length)]};
}
static NSDictionary *IPSTrackPresentation(void){
    NSString *video=IPSVideo();id core=IPSReadObjectIvar(IPSPlayer,"_playbackController");
    id response=IPSObject(IPSObject(core,@"contentPlaybackData"),@"playerResponse");
    if(!response)response=IPSObject(core,@"playerResponse");
    id details=[IPSObject(response,@"videoID") isEqual:video]?IPSObject(IPSObject(response,@"playerData"),@"videoDetails"):nil;
    NSString *title=IPSString(IPSObject(details,@"title")),*artist=IPSString(IPSObject(details,@"author"));
    if(!title.length)title=IPSString(IPSMetadata[video][@"title"]);
    if(!title.length)for(NSDictionary *track in IPSConfig[@"tracks"])if([track[@"id"] isEqual:video]){title=IPSString(track[@"title"]);break;}
    return @{@"videoId":video,@"title":title.length?title:video,@"artist":artist};
}

// When the host app removes its watch page, keep local controls available.
@interface IPSLocalPlayerBar : UIView
@property UIImageView *cover;
@property UILabel *titleLabel,*detailLabel;
@property UIButton *playButton;
- (void)toggle;
@end
@implementation IPSLocalPlayerBar
- (instancetype)initWithFrame:(CGRect)frame {
    if((self=[super initWithFrame:frame])){
        self.backgroundColor=[UIColor colorWithWhite:0.10 alpha:0.98];self.layer.cornerRadius=12;self.clipsToBounds=YES;
        self.cover=[[UIImageView alloc] initWithFrame:CGRectMake(8,8,56,56)];self.cover.contentMode=UIViewContentModeScaleAspectFill;self.cover.clipsToBounds=YES;[self addSubview:self.cover];
        self.titleLabel=UILabel.new;self.titleLabel.font=[UIFont boldSystemFontOfSize:15];self.titleLabel.textColor=UIColor.whiteColor;[self addSubview:self.titleLabel];
        self.detailLabel=UILabel.new;self.detailLabel.font=[UIFont systemFontOfSize:12];self.detailLabel.textColor=UIColor.lightGrayColor;[self addSubview:self.detailLabel];
        self.playButton=[UIButton buttonWithType:UIButtonTypeSystem];self.playButton.tintColor=UIColor.whiteColor;[self.playButton addTarget:self action:@selector(toggle) forControlEvents:UIControlEventTouchUpInside];[self addSubview:self.playButton];
        self.accessibilityLabel=@"IPS player";
    }return self;
}
- (void)layoutSubviews {[super layoutSubviews];CGFloat width=self.bounds.size.width;self.titleLabel.frame=CGRectMake(76,12,MAX(0,width-136),24);self.detailLabel.frame=CGRectMake(76,38,MAX(0,width-136),20);self.playButton.frame=CGRectMake(width-56,8,48,56);}
- (void)toggle {IPSRequestToggle();IPSPublishPresentation(YES);}
@end
static IPSLocalPlayerBar *IPSLocalBar;
static void IPSReleasePresentation(void){
    IPSLocalBar.hidden=YES;
    for(NSArray *target in IPSSystemCommandTargets)[(MPRemoteCommand *)target[0] removeTarget:target[1]];
    IPSSystemCommandTargets=nil;
    NSDictionary *current=MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo;
    if(IPSCurrentSystemInfo && [current[MPNowPlayingInfoPropertyExternalContentIdentifier] isEqual:IPSPresentedVideo]){IPSPublishingSystemInfo=YES;@try{MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo=nil;}@finally{IPSPublishingSystemInfo=NO;}}
    IPSCurrentSystemInfo=nil;IPSPresentedVideo=nil;IPSPresentedImage=nil;[IPSArtworkTask cancel];
}
static void IPSRefreshLocalBar(BOOL playing){
    UIWindow *window=nil;
    for(UIWindow *candidate in UIApplication.sharedApplication.windows)if(candidate.isKeyWindow){window=candidate;break;}
    UIViewController *host=IPSWatchHost;
    BOOL nativeVisible=[host isKindOfClass:UIViewController.class] && host.isViewLoaded && host.view.window==window;
    if(nativeVisible)for(UIView *view=host.view;view;view=view.superview)if(view.hidden || view.alpha<0.01){nativeVisible=NO;break;}
    BOOL show=IPSHoldActive() && !IPSCrossRunning() && !IPSInactive && UIApplication.sharedApplication.applicationState==UIApplicationStateActive && window && !nativeVisible;
    if(!show){IPSLocalBar.hidden=YES;return;}
    if(!IPSLocalBar)IPSLocalBar=[[IPSLocalPlayerBar alloc] initWithFrame:CGRectZero];
    if(IPSLocalBar.superview!=window){[IPSLocalBar removeFromSuperview];[window addSubview:IPSLocalBar];}
    IPSLocalBar.frame=CGRectMake(12,MAX(window.safeAreaInsets.top,window.bounds.size.height-window.safeAreaInsets.bottom-132),window.bounds.size.width-24,72);
    IPSLocalBar.hidden=NO;IPSLocalBar.titleLabel.text=IPSPresentedTitle;IPSLocalBar.cover.image=IPSPresentedImage;
    double position=MAX(0,IPSProbeTime(IPSPlayer)),duration=IPSDuration(IPSPlayer);NSInteger left=(NSInteger)MAX(0,ceil(duration-position));
    IPSLocalBar.detailLabel.text=[NSString stringWithFormat:@"IPS · %@ · %ld:%02ld resterend",playing?@"Speelt":@"Gepauzeerd",(long)(left/60),(long)(left%60)];
    [IPSLocalBar.playButton setImage:[UIImage systemImageNamed:playing?@"pause.fill":@"play.fill"] forState:UIControlStateNormal];
    IPSLocalBar.playButton.accessibilityLabel=playing?@"Pauzeer IPS":@"Hervat IPS";
}
static void IPSLoadPresentationImage(NSString *video){
    [IPSArtworkTask cancel];IPSArtworkTask=nil;
    if(!IPSArtworkCache){IPSArtworkCache=NSCache.new;IPSArtworkCache.countLimit=6;}
    IPSPresentedImage=[IPSArtworkCache objectForKey:video];if(IPSPresentedImage)return;
    if(!IPSArtworkSession){NSURLSessionConfiguration *config=NSURLSessionConfiguration.ephemeralSessionConfiguration;config.HTTPShouldSetCookies=NO;config.HTTPCookieStorage=nil;config.URLCache=nil;IPSArtworkSession=[NSURLSession sessionWithConfiguration:config];}
    // No player token, account cookies, or server headers go to the image CDN.
    NSURL *url=[NSURL URLWithString:[NSString stringWithFormat:@"https://i.ytimg.com/vi/%@/hqdefault.jpg",video]];
    NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:url];request.timeoutInterval=6;
    NSUInteger generation=IPSGeneration;
    IPSArtworkTask=[IPSArtworkSession dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
        if(error || [(NSHTTPURLResponse *)response statusCode]!=200 || data.length>2097152)return;
        UIImage *image=[UIImage imageWithData:data];if(!image)return;
        dispatch_async(dispatch_get_main_queue(),^{if(generation!=IPSGeneration || ![video isEqual:IPSPresentedVideo] || !IPSHoldActive())return;[IPSArtworkCache setObject:image forKey:video];IPSPresentedImage=image;IPSPublishPresentation(YES);});
    }];[IPSArtworkTask resume];
}
static void IPSPublishPresentation(BOOL force){
    if(!IPSHoldActive() || IPSInactive){IPSReleasePresentation();return;}
    NSString *video=IPSVideo();if(!IPSMatch(video,@"^[A-Za-z0-9_-]{11}$"))return;
    BOOL changed=![video isEqual:IPSPresentedVideo];
    if(changed){IPSPresentedVideo=video;IPSPresentedTitle=nil;IPSPresentedArtist=nil;IPSLoadPresentationImage(video);}
    NSDictionary *track=IPSTrackPresentation();NSString *title=track[@"title"],*artist=track[@"artist"];
    BOOL metadataChanged=![title isEqual:IPSPresentedTitle] || ![artist isEqual:IPSPresentedArtist];
    IPSPresentedTitle=title;IPSPresentedArtist=artist;
    if(UIApplication.sharedApplication.applicationState==UIApplicationStateActive){
        UIViewController *host=IPSWatchHost;if(host.isViewLoaded)IPSRefreshActualCovers(host.view);
        IPSRefreshSurface();
    }
    BOOL playing=NO;if(!IPSActualPlaying(&playing))playing=IPSKnownPlaying && IPSPlaying;
    if(IPSLocalPlaybackPaused())playing=NO;
    double now=NSDate.date.timeIntervalSince1970;
    if(!force && !changed && !metadataChanged && playing==IPSLastPublishedPlaying && now-IPSLastPublish<4){IPSRefreshLocalBar(playing);return;}
    NSMutableDictionary *info=[@{MPMediaItemPropertyTitle:title,MPMediaItemPropertyArtist:artist,
        MPMediaItemPropertyPlaybackDuration:@(IPSDuration(IPSPlayer)),MPNowPlayingInfoPropertyElapsedPlaybackTime:@(MAX(0,IPSProbeTime(IPSPlayer))),
        MPNowPlayingInfoPropertyPlaybackRate:@(playing?1.0:0.0),MPNowPlayingInfoPropertyDefaultPlaybackRate:@1.0,
        MPNowPlayingInfoPropertyExternalContentIdentifier:video,MPNowPlayingInfoPropertyMediaType:@(MPNowPlayingInfoMediaTypeAudio),MPNowPlayingInfoPropertyIsLiveStream:@NO} mutableCopy];
    if(IPSPresentedImage){UIImage *image=IPSPresentedImage;info[MPMediaItemPropertyArtwork]=[[MPMediaItemArtwork alloc] initWithBoundsSize:image.size requestHandler:^UIImage *(CGSize size){return image;}];}
    IPSPublishingSystemInfo=YES;
    @try{
        MPNowPlayingInfoCenter.defaultCenter.nowPlayingInfo=info;IPSCurrentSystemInfo=info;
        IPSBindSystemCommands();
        MPRemoteCommandCenter *commands=MPRemoteCommandCenter.sharedCommandCenter;
        commands.playCommand.enabled=YES;commands.pauseCommand.enabled=YES;commands.togglePlayPauseCommand.enabled=YES;
    }
    @finally{IPSPublishingSystemInfo=NO;}
    IPSLastPublish=now;IPSLastPublishedPlaying=playing;
    if(changed || metadataChanged || force){
        id host=IPSWatchHost;SEL setter=NSSelectorFromString(@"setVideoTitle:videoArtist:");
        @try{
            id nativeTitle=IPSNativeTitleText(title),nativeArtist=IPSNativeFormattedText(artist);
            if(nativeTitle && nativeArtist && [host respondsToSelector:setter])((void(*)(id,SEL,id,id))objc_msgSend)(host,setter,nativeTitle,nativeArtist);
        }@catch(NSException *error){IPSCrossInfo=[@"Titelweergave overgeslagen: " stringByAppendingString:error.reason ?: @"onbekend"];}
        IPSRefreshSurface();
    }
    IPSRefreshLocalBar(playing);
}
static void IPSConfigureBackgroundAudio(void){
    BOOL background=[[[IPSD() dictionaryForKey:@"YTMUltimate"] objectForKey:@"backgroundPlayback"] boolValue];
    if(!IPSHoldActive() || !background || IPSBackgroundAudioReady || IPSLocalPlaybackPaused())return;
    AVAudioSession *audio=AVAudioSession.sharedInstance;NSError *error=nil;
    if([audio.category isEqual:AVAudioSessionCategoryPlayback]){IPSBackgroundAudioReady=YES;return;}
    [audio setCategory:AVAudioSessionCategoryPlayback error:&error];
    if(!error){[audio setActive:YES error:&error];IPSBackgroundAudioReady=error==nil;}
}
static void IPSPresentationPulse(void){
    @try{
        IPSMonitor();IPSTick();
        if(IPSHoldActive()){
            IPSConfigureBackgroundAudio();IPSPublishPresentation(NO);
            if(UIApplication.sharedApplication.applicationState==UIApplicationStateActive && !IPSCrossRunning() && NSDate.date.timeIntervalSince1970-IPSLastSurfaceRepair>4){
                IPSLastSurfaceRepair=NSDate.date.timeIntervalSince1970;
                if(IPSObject(IPSWatchHost,@"playerViewController")!=IPSPlayer)IPSBindSurface();
            }
        }else{IPSReleasePresentation();IPSRetainedWatch=nil;IPSBackgroundAudioReady=NO;}
        if(UIApplication.sharedApplication.applicationState==UIApplicationStateActive)IPSRefreshOverlay();
    }@catch(NSException *error){IPSCrossInfo=[@"Weergaveherstel: " stringByAppendingString:error.reason ?: @"onbekend"];}
}
static void IPSStartPresentation(void){
    // Dispatch timers do not depend on a UIKit run-loop tracking/lock mode.
    if(IPSPresentationClock)return;
    IPSExitTask=UIBackgroundTaskInvalid;
    IPSPresentationClock=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,dispatch_get_main_queue());
    dispatch_source_set_timer(IPSPresentationClock,DISPATCH_TIME_NOW,2*NSEC_PER_SEC,NSEC_PER_SEC/4);
    dispatch_source_set_event_handler(IPSPresentationClock,^{IPSPresentationPulse();});dispatch_resume(IPSPresentationClock);
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note){
        if(!IPSEnabled())return;IPSConfigureBackgroundAudio();IPSPublishPresentation(YES);IPSNextPoll=0;IPSTick();
        // One finite handover task per foreground/background transition, not a
        // renewed silent-audio or indefinite background keepalive.
        if(IPSExitTask==UIBackgroundTaskInvalid)IPSExitTask=[UIApplication.sharedApplication beginBackgroundTaskWithName:@"IPS control handover" expirationHandler:^{UIBackgroundTaskIdentifier task=IPSExitTask;IPSExitTask=UIBackgroundTaskInvalid;if(task!=UIBackgroundTaskInvalid)[UIApplication.sharedApplication endBackgroundTask:task];}];
    }];
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *note){
        if(IPSExitTask!=UIBackgroundTaskInvalid){[UIApplication.sharedApplication endBackgroundTask:IPSExitTask];IPSExitTask=UIBackgroundTaskInvalid;}
        if(!IPSEnabled())return;
        if(IPSHoldActive() && !IPSCrossRunning())IPSBindSurface();
        IPSNextPoll=0;IPSPresentationPulse();IPSPublishPresentation(YES);
    }];
}
