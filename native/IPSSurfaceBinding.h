// Native UI reattachment, without watch URLs or loading another playback.
static __weak id IPSWatchHost;
static id IPSRetainedWatch;
static BOOL IPSBindingSurface;
static char IPSArtworkVideoKey;
@interface IPSArtworkLoader : NSObject
- (instancetype)initWithImageView:(id)view URL:(id)url;
- (instancetype)initWithImageView:(id)view thumbnail:(id)thumbnail;
@end
static id IPSReadObjectIvar(id object,const char *name){Ivar field=class_getInstanceVariable([object class],name);return field?object_getIvar(object,field):nil;}
// The watch setter takes YTIFormattedString, despite its generic '@' ABI.
// Its implementation immediately sends stringWithFormattingRemoved.
static id IPSNativeFormattedText(NSString *text){
    Class type=NSClassFromString(@"YTIFormattedString");SEL factory=NSSelectorFromString(@"formattedStringWithString:");
    NSMethodSignature *signature=[type methodSignatureForSelector:factory];
    if(![text isKindOfClass:NSString.class] || !signature || signature.numberOfArguments!=3 || signature.methodReturnType[0]!='@')return nil;
    id formatted=((id(*)(id,SEL,id))objc_msgSend)(type,factory,text);
    if(![formatted isKindOfClass:type] || ![formatted respondsToSelector:NSSelectorFromString(@"stringWithFormattingRemoved")])return nil;
    return formatted;
}
static id IPSNativeTitleText(NSString *text){
    // 8.x stores the title directly in UILabel; only artist is formatted there.
    NSString *version=[NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"];
    return [version hasPrefix:@"8."]?text:IPSNativeFormattedText(text);
}
static NSInteger IPSPresentedPlayerState(NSInteger fallback){
    SEL selector=NSSelectorFromString(@"playerState");NSMethodSignature *sig=[IPSPlayer methodSignatureForSelector:selector];
    if(!IPSHoldActive() || !sig || sig.numberOfArguments!=2 || strcmp(sig.methodReturnType,@encode(NSInteger)))return fallback;
    return ((NSInteger(*)(id,SEL))objc_msgSend)(IPSPlayer,selector);
}
static BOOL IPSUpdateArtwork(id cell){
    if(!IPSHoldActive())return NO;
    NSString *video=IPSVideo();if(!IPSMatch(video,@"^[A-Za-z0-9_-]{11}$"))return NO;
    id image=IPSReadObjectIvar(cell,"_thumbnailView");Ivar field=class_getInstanceVariable([cell class],"_thumbnailController");
    Class controller=NSClassFromString(@"YTThumbnailController");
    if(!image || !field || !controller)return NO;
    if([objc_getAssociatedObject(cell,&IPSArtworkVideoKey) isEqual:video])return YES;
    @try{
        id core=IPSReadObjectIvar(IPSPlayer,"_playbackController");id data=IPSObject(core,@"contentPlaybackData");
        id response=IPSObject(data,@"playerResponse");if(!response)response=IPSObject(core,@"playerResponse");
        id proto=IPSObject(response,@"playerData");id details=IPSObject(proto,@"videoDetails");
        id thumbnail=[IPSObject(response,@"videoID") isEqual:video]?IPSObject(details,@"thumbnail"):nil;
        SEL init=NSSelectorFromString(thumbnail?@"initWithImageView:thumbnail:":@"initWithImageView:URL:");
        id source=thumbnail ?: [NSURL URLWithString:[NSString stringWithFormat:@"https://i.ytimg.com/vi/%@/hqdefault.jpg",video]];
        IPSArtworkLoader *allocated=[controller alloc];NSMethodSignature *sig=[allocated methodSignatureForSelector:init];
        if(!sig || sig.numberOfArguments!=4 || sig.methodReturnType[0]!='@')return NO;
        id replacement=thumbnail?[allocated initWithImageView:image thumbnail:source]:[allocated initWithImageView:image URL:source];
        if(!replacement)return NO;
        IPSInvoke(object_getIvar(cell,field),@"reset");
        object_setIvar(cell,field,replacement);
        objc_setAssociatedObject(cell,&IPSArtworkVideoKey,video,OBJC_ASSOCIATION_COPY_NONATOMIC);
        IPSInvoke(replacement,@"loadImageIfNeeded");return YES;
    }@catch(NSException *error){return NO;}
}
static void IPSRefreshArtworkTree(UIView *view){
    if(!view)return;
    Class cell=NSClassFromString(@"YTMPlayerCarouselCell");
    if(cell && [view isKindOfClass:cell])IPSUpdateArtwork(view);
    for(UIView *child in view.subviews)IPSRefreshArtworkTree(child);
}
static void IPSRememberWatch(id host,id player){if(player && !IPSProbeConstructing && (!IPSWatchHost || player==IPSPlayer || player==IPSVisibleHost))IPSWatchHost=host;}
static id IPSFindWatch(UIViewController *root){
    if(!root)return nil;
    Class watch=NSClassFromString(@"YTMWatchViewController");
    if(watch && [root isKindOfClass:watch]){id player=IPSObject(root,@"playerViewController");if(player==IPSPlayer || player==IPSVisibleHost)return root;}
    id found=IPSFindWatch(root.presentedViewController);if(found)return found;
    for(UIViewController *child in root.childViewControllers){found=IPSFindWatch(child);if(found)return found;}
    return nil;
}
static void IPSRefreshSurface(void){
    if(!IPSHoldActive())return;
    BOOL playing=NO;BOOL known=IPSActualPlaying(&playing);if(IPSLocalPlaybackPaused()){playing=NO;known=YES;}
    if(known && [IPSNowPlaying respondsToSelector:NSSelectorFromString(@"setPlaying:")])IPSSetBool(IPSNowPlaying,@"setPlaying:",playing);
    for(NSArray *pair in @[@[@"setScrubberTotalTime:",@(IPSDuration(IPSPlayer))],@[@"setPlaybackMediaTime:",@(MAX(0,IPSProbeTime(IPSPlayer)))],@[@"setScrubberMediaTime:",@(MAX(0,IPSProbeTime(IPSPlayer)))]]){
        SEL sel=NSSelectorFromString(pair[0]);NSMethodSignature *sig=[IPSNowPlaying methodSignatureForSelector:sel];
        if(sig && sig.numberOfArguments==3 && !strcmp([sig getArgumentTypeAtIndex:2],@encode(double)) && !strcmp(sig.methodReturnType,@encode(void)))((void(*)(id,SEL,double))objc_msgSend)(IPSNowPlaying,sel,[pair[1] doubleValue]);
    }
}
static void IPSBindSurface(void){
    if(!IPSHoldActive() || IPSBindingSurface)return;
    id host=IPSWatchHost;id previous=IPSObject(host,@"playerViewController");if(previous && previous!=IPSVisibleHost && previous!=IPSPlayer)host=nil;
    if(!host){for(UIWindow *window in UIApplication.sharedApplication.windows){if(window.isKeyWindow){host=IPSFindWatch(window.rootViewController);break;}}}
    if(!host){IPSCrossInfo=@"Overgang klaar; zichtbare player nog niet gevonden.";return;}
    SEL attach=NSSelectorFromString(@"attachPlayerViewController:"),detach=NSSelectorFromString(@"detachPlayerViewController");
    NSMethodSignature *a=[host methodSignatureForSelector:attach],*d=[host methodSignatureForSelector:detach];
    if(!a || a.numberOfArguments!=3 || strcmp(a.methodReturnType,@encode(void)) || !d || d.numberOfArguments!=2 || d.methodReturnType[0]!='@'){IPSCrossInfo=@"Overgang klaar; UI-koppeling niet beschikbaar.";return;}
    IPSBindingSurface=YES;
    @try{
        if(IPSObject(host,@"playerViewController")!=IPSPlayer){
            ((id(*)(id,SEL))objc_msgSend)(host,detach);
            ((void(*)(id,SEL,id))objc_msgSend)(host,attach,IPSPlayer);
        }
        IPSVisibleHost=IPSPlayer;IPSWatchHost=host;IPSRetainedWatch=host;
        // Re-publish the already loaded data to the observers just attached.
        Ivar field=class_getInstanceVariable([IPSPlayer class],"_playbackController");id core=field?object_getIvar(IPSPlayer,field):nil;
        id video=IPSObject(core,@"activeVideo"),data=IPSObject(core,@"contentPlaybackData");
        SEL notify=NSSelectorFromString(@"notifyEventsDelegateAboutNewActiveVideo:withPlaybackData:");NSMethodSignature *m=[IPSPlayer methodSignatureForSelector:notify];
        if(video && data && m && m.numberOfArguments==4 && !strcmp(m.methodReturnType,@encode(void)))((void(*)(id,SEL,id,id))objc_msgSend)(IPSPlayer,notify,video,data);
        if(IPSLocalPlaybackPaused())IPSInvoke(IPSPlayer,@"pause");
        IPSInvoke(core,@"updateLockScreenController");IPSRefreshSurface();
        if([host isKindOfClass:UIViewController.class])IPSRefreshArtworkTree(((UIViewController *)host).view);
    }@catch(NSException *error){IPSCrossInfo=[@"Audio blijft actief; UI-koppeling: " stringByAppendingString:error.reason ?: @"onbekend"];}
    @finally{IPSBindingSurface=NO;}
}
