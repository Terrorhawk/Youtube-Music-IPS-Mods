// Direct A/B audio route; presentation is synchronized separately in V0.5.
static BOOL IPSAuxiliaryEvent, IPSInternalSeek;
static NSDictionary *IPSMainNowPlayingInfo;
static NSString *IPSMainNowPlayingVideo;
static void IPSRepairNowPlaying(void) {
    IPSPublishPresentation(YES);
}

static NSString *IPSLastCrossFailure;
static NSDate *IPSLastCrossFailureAt;
static void IPSRecordCrossFailure(void) {IPSLastCrossFailure=[IPSCrossInfo copy];IPSLastCrossFailureAt=NSDate.date;}

@interface IPSDirectTransition : NSObject
- (instancetype)initWithNavigationEndpoint:(id)endpoint latencyLogger:(id)logger;
@end
@interface IPSDirectPlayback : NSObject
+ (instancetype)initWithPlayerTransition:(id)transition playbackConfig:(id)config;
+ (instancetype)playbackWithPlayerTransition:(id)transition playbackConfig:(id)config;
@end
static NSInteger IPSQueuePhase; // 1 load B, 2 B paused at start, 3 start B, 4 mix.
static NSString *IPSQueueTarget,*IPSQueueOutgoing,*IPSQueueCommand;
static NSTimer *IPSQueueTimer;
static id IPSQueueCoordinator,IPSOutgoing,IPSOutgoingCoordinator;
static id IPSOwnedActive,IPSOwnedCoordinator;
static __weak id IPSVisibleHost;
static float IPSQueueGain;
static BOOL IPSOutgoingEnded;
static BOOL IPSCrossRunning(void){return IPSQueuePhase!=0;}
static BOOL IPSHoldActive(void){return IPSEnabled() && IPSOwnedActive!=nil;}
static BOOL IPSBool(id obj,NSString *name,BOOL *value){
    SEL s=NSSelectorFromString(name);NSMethodSignature *m=[obj methodSignatureForSelector:s];
    if(!m || m.numberOfArguments!=2 || strcmp(m.methodReturnType,@encode(BOOL)))return NO;
    *value=((BOOL(*)(id,SEL))objc_msgSend)(obj,s);return YES;
}
static BOOL IPSActualPlaying(BOOL *playing){
    if(IPSLocalPlaybackPaused()){*playing=NO;return YES;}
    Ivar field=class_getInstanceVariable([IPSPlayer class],"_playbackController");id core=field?object_getIvar(IPSPlayer,field):nil;
    if(IPSBool(core,@"isPlayingContentVideo",playing))return YES;
    id audio=IPSActiveAudioPlayer(IPSPlayer);SEL s=NSSelectorFromString(@"rate");NSMethodSignature *m=[audio methodSignatureForSelector:s];
    if(!m || m.numberOfArguments!=2 || strcmp(m.methodReturnType,@encode(float)))return NO;
    float rate=((float(*)(id,SEL))objc_msgSend)(audio,s);if(!isfinite(rate))return NO;*playing=rate>0;return YES;
}
static double IPSDuration(id player){SEL s=NSSelectorFromString(@"currentVideoTotalMediaTime");NSMethodSignature *m=[player methodSignatureForSelector:s];if(!m || m.numberOfArguments!=2 || strcmp(m.methodReturnType,@encode(double)))return 0;double duration=((double(*)(id,SEL))objc_msgSend)(player,s);return isfinite(duration) && duration>0?duration:0;}
static BOOL IPSShouldMute(id player){return (IPSProbeConstructing && player!=IPSPlayer) || (player==IPSProbeSecondary && IPSSecondaryMuted);}
static void IPSMarkEnded(NSString *video){IPSEndedID=video;IPSEndedEvent=NSUUID.UUID.UUIDString;IPSPlaying=NO;IPSKnownPlaying=YES;IPSNextPoll=0;}
static BOOL IPSDualHandleEnd(id player){
    if(IPSCrossRunning() && player==IPSOutgoing){IPSOutgoingEnded=YES;return YES;}
    return IPSHoldActive() && player!=IPSPlayer;
}
static void IPSDualReturnToUI(void){
    if(IPSOwnedActive){IPSAuxiliaryEvent=YES;IPSInvoke(IPSOwnedActive,@"pause");IPSAuxiliaryEvent=NO;IPSOwnedActive=nil;IPSOwnedCoordinator=nil;if(IPSVisibleHost)IPSPlayer=IPSVisibleHost;}
}
static void IPSSetBool(id object,NSString *name,BOOL value){
    SEL s=NSSelectorFromString(name);NSMethodSignature *m=[object methodSignatureForSelector:s];
    if(!m || m.numberOfArguments!=3 || strcmp(m.methodReturnType,@encode(void)) || strcmp([m getArgumentTypeAtIndex:2],@encode(BOOL)))@throw [NSException exceptionWithName:@"IPS" reason:name userInfo:nil];
    ((void(*)(id,SEL,BOOL))objc_msgSend)(object,s,value);
}
static id IPSDirectPlaybackFor(NSString *target){
    Class commands=NSClassFromString(@"YTICommand");SEL make=NSSelectorFromString(@"watchNavigationEndpointWithPlaylistID:videoID:index:watchNextToken:");
    NSMethodSignature *m=[commands methodSignatureForSelector:make];
    if(!m || m.numberOfArguments!=6 || m.methodReturnType[0]!='@' || strcmp([m getArgumentTypeAtIndex:4],@encode(NSUInteger)))@throw [NSException exceptionWithName:@"IPS" reason:@"Watch-endpointfactory ontbreekt" userInfo:nil];
    id endpoint=((id(*)(id,SEL,id,id,NSUInteger,id))objc_msgSend)(commands,make,nil,target,0,nil);
    id previous=IPSProbePlaybacks.lastObject;
    id logger=IPSObject(IPSObject(previous,@"playerTransition"),@"latencyLogger");
    Class transitions=NSClassFromString(@"YTPlayerTransition");SEL init=@selector(initWithNavigationEndpoint:latencyLogger:);
    m=[transitions instanceMethodSignatureForSelector:init];
    if(!m || m.numberOfArguments!=4 || m.methodReturnType[0]!='@')@throw [NSException exceptionWithName:@"IPS" reason:@"Transitionfactory ontbreekt" userInfo:nil];
    id transition=[(IPSDirectTransition *)[transitions alloc] initWithNavigationEndpoint:endpoint latencyLogger:logger];
    if(![IPSObject(transition,@"videoID") isEqual:target])@throw [NSException exceptionWithName:@"IPS" reason:@"Transition bevat niet de gekozen opvolger" userInfo:nil];
    id config=IPSObject(NSClassFromString(@"YTPlaybackConfig"),@"defaultConfig");
    if(!config)@throw [NSException exceptionWithName:@"IPS" reason:@"Playbackconfig ontbreekt" userInfo:nil];
    IPSSetBool(config,@"setStartPlayback:",NO);IPSSetBool(config,@"setLoadAsActivePlayback:",YES);if([config respondsToSelector:NSSelectorFromString(@"setAutoTransitionToNextPlayback:")])IPSSetBool(config,@"setAutoTransitionToNextPlayback:",NO);
    Class playbacks=NSClassFromString(@"YTPlayerPlayback");
    if([playbacks respondsToSelector:@selector(playbackWithPlayerTransition:playbackConfig:)])return [(id)playbacks playbackWithPlayerTransition:transition playbackConfig:config];
    if([playbacks respondsToSelector:@selector(initWithPlayerTransition:playbackConfig:)])return [(id)playbacks initWithPlayerTransition:transition playbackConfig:config];
    @throw [NSException exceptionWithName:@"IPS" reason:@"Playbackfactory ontbreekt" userInfo:nil];
}
static void IPSQueueCancel(BOOL failed){
    if(!IPSQueuePhase)return;
    BOOL promoted=IPSQueuePhase==4;
    [IPSQueueTimer invalidate];IPSQueueTimer=nil;IPSQueuePhase=0;IPSSecondaryMuted=NO;
    BOOL ended=IPSOutgoingEnded;BOOL nativeEnd=NO;if(IPSBool(IPSOutgoing,@"isPlaybackFinished",&nativeEnd))ended|=nativeEnd;
    IPSAuxiliaryEvent=YES;
    if(promoted){IPSInvoke(IPSOutgoing,@"pause");}
    else{IPSInvoke(IPSProbeSecondary,@"pause");}
    IPSAuxiliaryEvent=NO;
    if(IPSQueueGain>0)IPSVolume(IPSPlayer,&IPSQueueGain,YES);
    IPSProbeSecondary=nil;IPSProbePrimary=nil;IPSOutgoing=nil;IPSOutgoingCoordinator=nil;IPSQueueCoordinator=nil;
    if(ended && !promoted)IPSMarkEnded(IPSQueueOutgoing);
    if(!promoted)IPSPendingLoadCommand=nil;
    if(failed && !promoted){
        IPSRecordCrossFailure();IPSDesiredID=IPSQueueOutgoing;[IPSD() setObject:IPSDesiredID ?: @"" forKey:@"IPS.desiredVideoId"];IPSLoadAt=0;
        IPSRequest(@"crossfade-failed",@{@"id":IPSQueueCommand ?: @"",@"outgoingEnded":@(ended)},^(NSDictionary *reply){
            if(IPSEnabled() && !IPSInactive && reply){IPSCommand(reply[@"command"]);IPSNextPoll=0;}
        });
    }
    if(promoted)IPSBindSurface();
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.3*NSEC_PER_SEC)),dispatch_get_main_queue(),^{IPSRepairNowPlaying();});
}
static BOOL IPSQueueReady(void){return [IPSD() boolForKey:@"IPS.queueCrossfade"] && IPSEnabled() && !IPSInactive && IPSProbeFactory && IPSProbePlaybacks.count && IPSPlayer && !IPSProbeTimer && !IPSQueuePhase;}
static NSDictionary *IPSCrossStatus(NSDictionary *input){
    NSMutableDictionary *s=[input mutableCopy];
    if(!IPSCrossRunning() && ([IPSCrossInfo hasPrefix:@"Nog geen"] || [IPSCrossInfo hasPrefix:@"Gereedheid:"]))IPSCrossInfo=[NSString stringWithFormat:@"Gereedheid directe B: IPS %@; serverproef %@; factory %@; playback %lu; player %@.",IPSEnabled()?@"aan":@"uit",[IPSD() boolForKey:@"IPS.queueCrossfade"]?@"aan":@"uit",IPSProbeFactory?@"ja":@"nee",(unsigned long)IPSProbePlaybacks.count,IPSPlayer?@"ja":@"nee"];
    s[@"crossfadeCapable"]=@(IPSQueueReady());s[@"crossfadeActive"]=@(IPSCrossRunning());s[@"crossfadeSeconds"]=@(IPSCrossSeconds());
    if(IPSQueuePhase && IPSQueuePhase<4){s[@"playbackState"]=IPSOutgoingEnded?@"ended":@"unknown";if(IPSOutgoingEnded){if(!IPSEndedEvent)IPSEndedEvent=NSUUID.UUID.UUIDString;s[@"endedVideoId"]=IPSQueueOutgoing ?: @"";s[@"endedEvent"]=IPSEndedEvent;}}
    return s;
}
static BOOL IPSQueueLoad(NSString *target,NSString *command){
    if(!IPSQueueReady() || !IPSMatch(target,@"^[A-Za-z0-9_-]{11}$")){IPSCrossInfo=@"Directe crossfade niet gereed";return NO;}
    float gain=NAN;if(!IPSVolume(IPSPlayer,&gain,NO) || gain<=0){IPSCrossInfo=[NSString stringWithFormat:@"Geen bruikbaar volume voor directe crossfade: %.4f",gain];return NO;}
    @try{
        if(!IPSOwnedActive)IPSVisibleHost=IPSPlayer;
        IPSOutgoing=IPSPlayer;IPSOutgoingCoordinator=IPSOwnedCoordinator;IPSQueueGain=gain;IPSQueueTarget=target;IPSQueueOutgoing=IPSVideo();IPSQueueCommand=command;IPSOutgoingEnded=NO;
        IPSQueuePhase=1;IPSSecondaryMuted=YES;IPSProbeConstructing=YES;
        BOOL external=YES;if(!IPSBool(IPSPlayer,@"isExternalPlaybackActive",&external) || external)@throw [NSException exceptionWithName:@"IPS" reason:@"Geen bevestigde lokale playback" userInfo:nil];
        IPSQueueCoordinator=[[NSClassFromString(@"YTLocalPlaybackCoordinator") alloc] init];
        Class cls=NSClassFromString(@"YTPlayerViewController");BOOL modern=IPSProbeFactory.count==5;
        IPSProbeSecondary=modern?[(IPSProbePlayer *)[cls alloc] initWithServiceRegistryScope:IPSProbeFactory[0] playerViewControllerTraits:IPSProbeFactory[1] parentResponder:IPSProbeFactory[2] overlayFactory:IPSProbeFactory[3] playbackCoordinator:IPSQueueCoordinator]:[(IPSProbePlayer *)[cls alloc] initWithServiceRegistryScope:IPSProbeFactory[0] parentResponder:IPSProbeFactory[1] overlayFactory:IPSProbeFactory[2] playbackCoordinator:IPSQueueCoordinator];
        if(!IPSProbeSecondary || !IPSQueueCoordinator)@throw [NSException exceptionWithName:@"IPS" reason:@"Tweede player ontbreekt" userInfo:nil];
        [IPSProbeSecondary loadViewIfNeeded];float zero=0;IPSVolume(IPSProbeSecondary,&zero,YES);
        id playback=IPSDirectPlaybackFor(target);
        SEL load=NSSelectorFromString(@"loadWithPlayerPlayback:");NSMethodSignature *m=[IPSProbeSecondary methodSignatureForSelector:load];
        if(!m || m.numberOfArguments!=3 || strcmp(m.methodReturnType,@encode(void)))@throw [NSException exceptionWithName:@"IPS" reason:@"Playback-load ontbreekt" userInfo:nil];
        ((void(*)(id,SEL,id))objc_msgSend)(IPSProbeSecondary,load,playback);
        IPSProbeConstructing=NO;
        double began=NSDate.date.timeIntervalSince1970;
        __block double startAt=0,startPosition=0,mixAt=0,fade=0;
        IPSCrossInfo=@"Direct B: gekozen opvolger stil voorbereiden; A blijft ongewijzigd spelen.";
        IPSQueueTimer=[NSTimer scheduledTimerWithTimeInterval:0.05 repeats:YES block:^(NSTimer *t){
          @try {
            double now=NSDate.date.timeIntervalSince1970;
            if(!IPSEnabled() || IPSInactive || ![IPSConfig[@"online"] boolValue] || ![IPSD() boolForKey:@"IPS.queueCrossfade"]){IPSQueueCancel(YES);return;}
            BOOL finished=NO;if(IPSBool(IPSOutgoing,@"isPlaybackFinished",&finished))IPSOutgoingEnded|=finished;
            if(IPSQueuePhase<4 && now-began>MAX(12,IPSCrossSeconds()+6)){IPSCrossInfo=@"Direct B overdracht bleef wachten; gewone overgang wordt aangevraagd.";IPSQueueCancel(YES);return;}
            double position=IPSProbeTime(IPSProbeSecondary);
            if(IPSQueuePhase==1){
                float silent=0;IPSVolume(IPSProbeSecondary,&silent,YES);
                id audio=IPSActiveAudioPlayer(IPSProbeSecondary);
                double duration=IPSDuration(IPSProbeSecondary);
                if([IPSObject(IPSProbeSecondary,@"currentVideoID") isEqual:target] && audio && duration>0 && position>=0 && position<=0.1){
                    IPSAuxiliaryEvent=YES;IPSInvoke(IPSProbeSecondary,@"pause");IPSAuxiliaryEvent=NO;
                    IPSQueuePhase=2;IPSCrossInfo=@"Direct B: opvolger geladen en stil op begin klaar; geen seeks.";
                }
                if(IPSQueuePhase==1 && (IPSOutgoingEnded || now-began>9)){IPSCrossInfo=[NSString stringWithFormat:@"Direct B niet klaar: id %@; duur %.2f; engine %@; tijd %.2f. Gewone overgang.",IPSObject(IPSProbeSecondary,@"currentVideoID") ?: @"ontbreekt",duration,NSStringFromClass([audio class]) ?: @"ontbreekt",position];IPSQueueCancel(YES);return;}

            }
            if(IPSQueuePhase==2){
                double total=IPSDuration(IPSOutgoing);double left=total>0?total-IPSProbeTime(IPSOutgoing):INFINITY;
                if(IPSQueueImmediate || IPSOutgoingEnded || left<=IPSCrossSeconds()){
                    IPSAuxiliaryEvent=YES;IPSInvoke(IPSProbeSecondary,@"play");IPSAuxiliaryEvent=NO;IPSQueuePhase=3;startAt=now;startPosition=position;
                }
            }
            if(IPSQueuePhase==3){
                if(position>startPosition+0.03 && [IPSObject(IPSProbeSecondary,@"currentVideoID") isEqual:target]){
                    double total=IPSDuration(IPSOutgoing);double left=total>0?total-IPSProbeTime(IPSOutgoing):INFINITY;
                    fade=IPSOutgoingEnded?0.05:(IPSQueueImmediate?IPSCrossSeconds():MAX(0.05,MIN(IPSCrossSeconds(),left)));
                    double incomingLeft=IPSDuration(IPSProbeSecondary)-position;if(incomingLeft>0)fade=MIN(fade,MAX(0.05,incomingLeft/2));
                    IPSOwnedActive=IPSProbeSecondary;IPSOwnedCoordinator=IPSQueueCoordinator;IPSPlayer=IPSOwnedActive;
                    IPSSecondaryMuted=NO;IPSQueuePhase=4;IPSActiveLoadCommand=IPSQueueCommand;IPSPendingLoadCommand=nil;mixAt=now;IPSKnownPlaying=YES;IPSPlaying=YES;IPSEndedID=nil;IPSEndedEvent=nil;IPSLoadAt=0;IPSProgressAt=now;
                    IPSSetBool(IPSPlayer,@"setActivePlayback:",YES);
                    Ivar f=class_getInstanceVariable([IPSPlayer class],"_playbackController");if(f)IPSInvoke(object_getIvar(IPSPlayer,f),@"updateLockScreenController");
                    float initial=gain*MIN(1,0.01/MAX(fade,0.05));float outgoing=gain-initial;IPSVolume(IPSOutgoing,&outgoing,YES);IPSVolume(IPSPlayer,&initial,YES);mixAt=now-0.01;
                    IPSCrossInfo=IPSOutgoingEnded?@"Direct B: oude track afgelopen; opvolger normaal gestart.":@"Direct B: A naar 0 en B naar vol; B wordt actieve player.";IPSNextPoll=0;
                }else if(now-startAt>3){IPSCrossInfo=@"Direct B startte niet; gewone overgang wordt aangevraagd.";IPSQueueCancel(YES);return;}
            }
            if(IPSQueuePhase==4){
                double amount=MIN(1,(now-mixAt)/fade);float a=gain*(1-amount),b=gain*amount;
                IPSVolume(IPSOutgoing,&a,YES);IPSVolume(IPSPlayer,&b,YES);
                if(amount>=1){IPSCrossInfo=@"Direct B: opvolger blijft actieve player; overgang klaar.";IPSQueueCancel(NO);IPSNextPoll=0;}
            }
          } @catch(NSException *error){IPSCrossInfo=[@"Direct B runtime: " stringByAppendingString:error.reason ?: @"onbekend"];IPSQueueCancel(YES);}
        }];
        [NSRunLoop.mainRunLoop addTimer:IPSQueueTimer forMode:NSRunLoopCommonModes];return YES;
    }@catch(NSException *e){IPSCrossInfo=[@"Direct B constructie: " stringByAppendingString:e.reason ?: @"onbekend"];IPSProbeConstructing=NO;IPSQueueCancel(NO);return NO;}
}
// Slider routing deliberately left to the native app in this experiment.
static id IPSSeekTarget(id sender){return nil;}
static void IPSOrdinaryLoadInfo(void){IPSCrossInfo=@"Gewone laadopdracht ontvangen.";if(IPSLastCrossFailure.length)IPSCrossInfo=[NSString stringWithFormat:@"%@\nLaatste crossfadefout (%@): %@",IPSCrossInfo,[NSDateFormatter localizedStringFromDate:IPSLastCrossFailureAt dateStyle:NSDateFormatterShortStyle timeStyle:NSDateFormatterMediumStyle],IPSLastCrossFailure];}
