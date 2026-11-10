// Shared native factory capture for the IPS player bridge.
static NSString *IPSCrossInfo=@"Nog geen overgang uitgevoerd.";
static NSArray *IPSProbeFactory;
static NSMutableArray *IPSProbePlaybacks;
static id IPSProbeSecondary;
static __weak id IPSProbePrimary;
static BOOL IPSProbeConstructing;
static BOOL IPSSecondaryMuted;
static NSTimer *IPSProbeTimer;
static float IPSProbeMainVolume, IPSProbeSecondVolume;
static double IPSCrossSeconds(void) {return MAX(1,MIN(8,IPSFadeSeconds(@"IPS.crossfadeSeconds")));}
static void IPSProbeRememberFactory(id scope,id parent,id overlay,id coordinator) {
    if (!IPSProbeConstructing && scope && parent && overlay && coordinator) IPSProbeFactory=@[scope,parent,overlay,coordinator];
}
static void IPSProbeRememberFactory9(id scope,id traits,id parent,id overlay,id coordinator) {
    if (!IPSProbeConstructing && scope && traits && parent && overlay && coordinator) IPSProbeFactory=@[scope,traits,parent,overlay,coordinator];
}
static void IPSProbeRememberPlayback(id player,id playback) {
    if (!playback || player==IPSProbeSecondary || IPSProbeConstructing) return;
    if (!IPSProbePlaybacks) IPSProbePlaybacks=NSMutableArray.array;
    if (IPSProbePlaybacks.lastObject==playback) return;
    [IPSProbePlaybacks addObject:playback];
    while (IPSProbePlaybacks.count>2) [IPSProbePlaybacks removeObjectAtIndex:0];
}
static double IPSProbeTime(id player) {
    SEL sel=NSSelectorFromString(@"currentVideoMediaTime");
    NSMethodSignature *sig=[player methodSignatureForSelector:sel];
    if (!sig || sig.numberOfArguments!=2 || strcmp(sig.methodReturnType,@encode(double))) return -1;
    return ((double(*)(id,SEL))objc_msgSend)(player,sel);
}
static void IPSProbeStop(void) {
    [IPSProbeTimer invalidate];IPSProbeTimer=nil;
    if (IPSProbeSecondary) {IPSInvoke(IPSProbeSecondary,@"pause");IPSVolume(IPSProbeSecondary,&IPSProbeSecondVolume,YES);}
    if (IPSProbePrimary) {IPSVolume(IPSProbePrimary,&IPSProbeMainVolume,YES);if(!IPSEnabled()) IPSInvoke(IPSProbePrimary,@"play");}
    IPSSecondaryMuted=NO;IPSProbeSecondary=nil;IPSProbePrimary=nil;
}
@interface IPSProbePlayer : UIViewController
- (id)initWithServiceRegistryScope:(id)scope playerViewControllerTraits:(id)traits parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator;
- (id)initWithServiceRegistryScope:(id)scope parentResponder:(id)parent overlayFactory:(id)overlay playbackCoordinator:(id)coordinator;
@end
