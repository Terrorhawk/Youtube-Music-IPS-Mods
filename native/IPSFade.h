// Opt-in, per-player fade. Never changes the device's system volume.
static NSTimer *IPSFadeTimer;
static __weak id IPSFadePlayer;
static float IPSFadeBase=1;
static BOOL IPSFadeHasBase, IPSFadeOutDone, IPSFadePending;
static NSString *IPSFadeVideo;
static NSTimer *IPSFadeWatchdog;
static double IPSFadeSeconds(NSString *key) {
    id value=[IPSD() objectForKey:key];
    double seconds=value ? [value doubleValue] : 3;
    return isfinite(seconds) ? MIN(8,MAX(0,seconds)) : 3;
}
static id IPSActiveAudioPlayer(id player) {
    Ivar field=class_getInstanceVariable([player class],"_playbackController");
    if(!field || ivar_getTypeEncoding(field)[0]!='@')return nil;
    id controller=object_getIvar(player,field);
    return IPSObject(IPSObject(controller,@"activeVideo"),@"mediaPlayer");
}
static BOOL IPSVolume(id player, float *value, BOOL write) {
    if(!write) {
        id audio=IPSActiveAudioPlayer(player);
        SEL get=NSSelectorFromString(@"volume");NSMethodSignature *signature=[audio methodSignatureForSelector:get];
        if(signature && signature.numberOfArguments==2 && !strcmp(signature.methodReturnType,@encode(float))) {
            float actual=((float(*)(id,SEL))objc_msgSend)(audio,get);
            if(isfinite(actual) && actual>=0 && actual<=1){*value=actual;return YES;}
        }
    }
    SEL selector=NSSelectorFromString(write ? @"setVolume:" : @"volume");
    NSMethodSignature *sig=[player methodSignatureForSelector:selector];
    if (!sig || sig.numberOfArguments!=(write?3:2)) return NO;
    if (write) {
        if (strcmp(sig.methodReturnType,@encode(void)) || strcmp([sig getArgumentTypeAtIndex:2],@encode(float))) return NO;
        ((void(*)(id,SEL,float))objc_msgSend)(player,selector,*value);
    } else {
        if (strcmp(sig.methodReturnType,@encode(float))) return NO;
        *value=((float(*)(id,SEL))objc_msgSend)(player,selector);
        if (!isfinite(*value) || *value<0 || *value>1) return NO;
    }
    return YES;
}
static void IPSFadeCancel(void) {
    [IPSFadeTimer invalidate]; IPSFadeTimer=nil;
    [IPSFadeWatchdog invalidate]; IPSFadeWatchdog=nil;
    if (IPSFadeHasBase) {float v=IPSFadeBase;IPSVolume(IPSFadePlayer,&v,YES);}
    IPSFadeHasBase=NO; IPSFadePlayer=nil;
    IPSFadePending=NO;
}
static void IPSFadeRamp(float start,float end,double seconds) {
    [IPSFadeTimer invalidate];
    [IPSFadeWatchdog invalidate];
    IPSFadeWatchdog=[NSTimer scheduledTimerWithTimeInterval:seconds+2 repeats:NO block:^(NSTimer *timer){IPSFadeCancel();}];
    NSTimeInterval began=NSDate.date.timeIntervalSince1970;
    IPSFadeTimer=[NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *timer){
        if (!IPSEnabled() || [IPSD() boolForKey:@"IPS.queueCrossfade"] || ![IPSD() boolForKey:@"IPS.fade"] || IPSFadePlayer!=IPSPlayer || IPSInactive) {IPSFadeCancel();return;}
        double fraction=MIN(1,(NSDate.date.timeIntervalSince1970-began)/MAX(seconds,0.1));
        float value=start+(end-start)*fraction;
        if (!IPSVolume(IPSFadePlayer,&value,YES)) {IPSFadeCancel();return;}
        if (fraction>=1) {[timer invalidate];IPSFadeTimer=nil;}
    }];
}
static void IPSFadeActivated(void) {
    if (IPSCrossRunning() || IPSHoldActive() || [IPSD() boolForKey:@"IPS.queueCrossfade"]) return;
    IPSFadeCancel(); IPSFadeOutDone=NO; IPSFadeVideo=IPSVideo();
    if (!IPSEnabled() || ![IPSD() boolForKey:@"IPS.fade"] || IPSInactive) return;
    float volume;
    if (!IPSVolume(IPSPlayer,&volume,NO)) return;
    // Recover a player left muted by the old fade, independently of UI state.
    // This is the app-player gain; the hardware output volume is untouched.
    if (volume<=0) {
        float remembered=[IPSD() floatForKey:@"IPS.lastFadeBase"];
        volume=remembered>0 && remembered<=1 ? remembered : 1;
        IPSVolume(IPSPlayer,&volume,YES);
    }
    [IPSD() setFloat:volume forKey:@"IPS.lastFadeBase"];
    IPSFadePlayer=IPSPlayer; IPSFadeBase=volume; IPSFadeHasBase=YES;
    IPSFadeWatchdog=[NSTimer scheduledTimerWithTimeInterval:12 repeats:NO block:^(NSTimer *timer){IPSFadeCancel();}];
    IPSFadePending=YES; // Start the ramp only once actual playback is observed.
}
static void IPSFadeUpdate(NSDictionary *status) {
    if (IPSCrossRunning() || [IPSD() boolForKey:@"IPS.queueCrossfade"] || !IPSEnabled() || ![IPSD() boolForKey:@"IPS.fade"] || IPSInactive) {IPSFadeCancel();return;}
    if (![status[@"playbackState"] isEqual:@"playing"]) {
        if (IPSFadeTimer) IPSFadeCancel();
        return;
    }
    if (![IPSVideo() isEqual:IPSFadeVideo]) {IPSFadeCancel();return;}
    if (IPSFadePending) {
        IPSFadePending=NO;
        double seconds=IPSFadeSeconds(@"IPS.fadeInSeconds");
        if (seconds>0) {float zero=0;IPSVolume(IPSPlayer,&zero,YES);IPSFadeRamp(0,IPSFadeBase,seconds);}
        return;
    }
    id duration=status[@"duration"], position=status[@"position"];
    if (![duration isKindOfClass:NSNumber.class] || ![position isKindOfClass:NSNumber.class]) return;
    double remaining=[duration doubleValue]-[position doubleValue];
    if (!IPSFadeOutDone && remaining>0 && remaining<=IPSFadeSeconds(@"IPS.fadeOutSeconds") && [duration doubleValue]>2*IPSFadeSeconds(@"IPS.fadeOutSeconds")) {
        float current;
        if (!IPSVolume(IPSPlayer,&current,NO)) return;
        if (!IPSFadeHasBase) {IPSFadePlayer=IPSPlayer;IPSFadeBase=current;IPSFadeHasBase=YES;}
        IPSFadeOutDone=YES;
        [IPSFadeWatchdog invalidate];
        IPSFadeWatchdog=[NSTimer scheduledTimerWithTimeInterval:remaining+2 repeats:NO block:^(NSTimer *timer){IPSFadeCancel();}];
        IPSFadeRamp(current,0,remaining);
    }
}
