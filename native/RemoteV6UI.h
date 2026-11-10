// Settings helpers: explicit confirmation before any QR capability is redeemed.
@interface IPSScanner : UIViewController <AVCaptureMetadataOutputObjectsDelegate>
@property AVCaptureSession *capture;
@property AVCaptureVideoPreviewLayer *preview;
@property (weak) IPSSettingsController *settings;
@property BOOL handled;
@end
@implementation IPSScanner
- (void)viewDidLoad {
    [super viewDidLoad]; self.view.backgroundColor=UIColor.blackColor;
    self.title=@"Scan de QR uit IPS-beheer";
    self.navigationItem.leftBarButtonItem=[[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemCancel target:self action:@selector(cancel)];
    [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted){
        dispatch_async(dispatch_get_main_queue(),^{
            if (!granted) { self.title=@"Geef cameratoegang in Instellingen"; return; }
            self.capture=AVCaptureSession.new; self.capture.sessionPreset=AVCaptureSessionPresetMedium;
            AVCaptureDeviceInput *input=[AVCaptureDeviceInput deviceInputWithDevice:[AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo] error:NULL];
            AVCaptureMetadataOutput *output=AVCaptureMetadataOutput.new;
            if (!input || ![self.capture canAddInput:input]) { self.title=@"Camera niet beschikbaar"; return; }
            [self.capture addInput:input];
            if (![self.capture canAddOutput:output]) return;
            [self.capture addOutput:output]; [output setMetadataObjectsDelegate:self queue:dispatch_get_main_queue()];
            output.metadataObjectTypes=@[AVMetadataObjectTypeQRCode];
            self.preview=[AVCaptureVideoPreviewLayer layerWithSession:self.capture]; self.preview.videoGravity=AVLayerVideoGravityResizeAspectFill;
            self.preview.frame=self.view.bounds; [self.view.layer insertSublayer:self.preview atIndex:0];
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{[self.capture startRunning];});
        });
    }];
}
- (void)viewDidLayoutSubviews { [super viewDidLayoutSubviews]; self.preview.frame=self.view.bounds; }
- (void)viewDidDisappear:(BOOL)animated { [super viewDidDisappear:animated]; [self.capture stopRunning]; }
- (void)cancel { [self.capture stopRunning]; [self dismissViewControllerAnimated:YES completion:nil]; }
- (void)captureOutput:(AVCaptureOutput *)output didOutputMetadataObjects:(NSArray *)objects fromConnection:(AVCaptureConnection *)connection {
    if (self.handled) return;
    for (AVMetadataMachineReadableCodeObject *object in objects) {
        NSURL *url=[NSURL URLWithString:object.stringValue ?: @""];
        if (![url.scheme isEqual:@"https"] || !url.host.length || url.user || url.password || url.query || ![url.path hasSuffix:@"/ips-pair"] || !IPSMatch(url.fragment,@"^[A-Za-z0-9_-]{43}$")) continue;
        self.handled=YES; [self.capture stopRunning];
        NSString *prefix=[url.path substringToIndex:url.path.length-9];
        NSURLComponents *parts=[NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
        parts.path=prefix; parts.fragment=nil;
        NSString *server=parts.URL.absoluteString;
        UIAlertController *alert=[UIAlertController alertControllerWithTitle:@"Met dit systeem koppelen?" message:server preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Annuleren" style:UIAlertActionStyleCancel handler:^(UIAlertAction *a){[self cancel];}]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Koppelen" style:UIAlertActionStyleDefault handler:^(UIAlertAction *a){
            NSMutableURLRequest *request=[NSMutableURLRequest requestWithURL:[NSURL URLWithString:[server stringByAppendingString:@"/api/ips/v1/pair/claim"]]];
            request.HTTPMethod=@"POST"; request.timeoutInterval=8;
            [request setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
            request.HTTPBody=[NSJSONSerialization dataWithJSONObject:@{@"code":url.fragment,@"playerId":IPSSetting(@"playerId")} options:0 error:NULL];
            [[IPSSession dataTaskWithRequest:request completionHandler:^(NSData *data,NSURLResponse *response,NSError *error){
                id parsed=data.length<16384 && [(NSHTTPURLResponse *)response statusCode]==200 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL] : nil;
                dispatch_async(dispatch_get_main_queue(),^{
                    if (![parsed isKindOfClass:NSDictionary.class] || !IPSMatch(parsed[@"eventId"],@"^[A-Za-z0-9_-]{1,64}$") || ![parsed[@"token"] isKindOfClass:NSString.class] || [parsed[@"token"] length]<24) { self.title=@"Koppelen mislukt. Maak een nieuwe QR."; return; }
                    self.settings.server.text=server; self.settings.event.text=parsed[@"eventId"];
                    self.settings.token.text=parsed[@"token"]; self.settings.enabled.on=YES;
                    [self dismissViewControllerAnimated:YES completion:nil];
                    // User reviews the event and saves using the existing keychain flow.
                });
            }] resume];
        }]];
        [self presentViewController:alert animated:YES completion:nil]; break;
    }
}
@end
static void IPSShowScanner(UIViewController *presenter) {
    IPSScanner *scanner=IPSScanner.new; scanner.settings=(IPSSettingsController *)presenter;
    UINavigationController *nav=[[UINavigationController alloc] initWithRootViewController:scanner];
    nav.modalPresentationStyle=UIModalPresentationFormSheet;
    [presenter presentViewController:nav animated:YES completion:nil];
}
@interface IPSFileImport : NSObject <UIDocumentPickerDelegate>
@property (weak) UIViewController *presenter;
@end
static IPSFileImport *IPSImporter;
@implementation IPSFileImport
- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *source=urls.firstObject; if (!source) return;
    BOOL scoped=[source startAccessingSecurityScopedResource];
    NSError *error=nil;
    NSDictionary *attrs=[NSFileManager.defaultManager attributesOfItemAtPath:source.path error:&error];
    if (!error && [attrs[NSFileSize] unsignedLongLongValue]>100*1024*1024) error=[NSError errorWithDomain:@"IPS" code:1 userInfo:@{NSLocalizedDescriptionKey:@"Kies een audiobestand kleiner dan 100 MB."}];
    AVAudioPlayer *probe=error ? nil : [[AVAudioPlayer alloc] initWithContentsOfURL:source error:&error];
    if (probe && [probe prepareToPlay]) {
        NSString *directory=[NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory,NSUserDomainMask,YES).firstObject stringByAppendingPathComponent:@"IPS"];
        [NSFileManager.defaultManager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:&error];
        NSString *temporary=[directory stringByAppendingPathComponent:@"emergency-import.audio"];
        NSString *target=[directory stringByAppendingPathComponent:@"emergency.audio"];
        [NSFileManager.defaultManager removeItemAtPath:temporary error:NULL];
        if (!error) [NSFileManager.defaultManager copyItemAtPath:source.path toPath:temporary error:&error];
        if (!error) {
            if ([NSFileManager.defaultManager fileExistsAtPath:target]) [NSFileManager.defaultManager replaceItemAtURL:[NSURL fileURLWithPath:target] withItemAtURL:[NSURL fileURLWithPath:temporary] backupItemName:nil options:0 resultingItemURL:NULL error:&error];
            else [NSFileManager.defaultManager moveItemAtPath:temporary toPath:target error:&error];
        }
    } else if (!error) error=[NSError errorWithDomain:@"IPS" code:2 userInfo:@{NSLocalizedDescriptionKey:@"Dit audiobestand kan niet worden afgespeeld."}];
    if (scoped) [source stopAccessingSecurityScopedResource];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(0.5*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
        UIAlertController *alert=[UIAlertController alertControllerWithTitle:error ? @"Import mislukt" : @"Noodnummer opgeslagen" message:error.localizedDescription ?: @"Dit bestand wordt herhaald als ook YouTube onbereikbaar is. Test dit vóór een event." preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self.presenter presentViewController:alert animated:YES completion:nil];
    });
}
@end
static void IPSChooseEmergency(UIViewController *presenter) {
    IPSImporter=IPSFileImport.new; IPSImporter.presenter=presenter;
    UIDocumentPickerViewController *picker=[[UIDocumentPickerViewController alloc] initWithDocumentTypes:@[@"public.audio"] inMode:UIDocumentPickerModeImport];
    picker.delegate=IPSImporter; picker.allowsMultipleSelection=NO;
    [presenter presentViewController:picker animated:YES completion:nil];
}
static __weak UIButton *IPSRemoteAction;
static const void *IPSActionKey=&IPSActionKey;
static const void *IPSHiddenKey=&IPSHiddenKey;
static NSHashTable *IPSActionRows;
static const void *IPSRefreshKey=&IPSRefreshKey;
static const void *IPSPinnedKey=&IPSPinnedKey;
static void IPSMaintainRemoteSlot(UIView *host) {
    UIButton *button=objc_getAssociatedObject(host,IPSPinnedKey);
    if (!button || !IPSEnabled() || button.superview!=host) return;
    button.frame=host.bounds;
    [host bringSubviewToFront:button];
}
static void IPSLayoutRemoteAction(UIView *row);
static void IPSQueueActionRefresh(UIView *row) {
    if (objc_getAssociatedObject(row,IPSRefreshKey)) return;
    objc_setAssociatedObject(row,IPSRefreshKey,@YES,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    __weak UIView *weakRow=row;
    dispatch_async(dispatch_get_main_queue(),^{
        UIView *current=weakRow; if (!current) return;
        objc_setAssociatedObject(current,IPSRefreshKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        IPSLayoutRemoteAction(current);
    });
}
static void IPSUpdateRemoteAction(void) {
    BOOL connected=IPSEnabled() && IPSLastContact && -IPSLastContact.timeIntervalSinceNow<12 && IPSFailures==0;
    BOOL inactive=connected && IPSInactive;
    [IPSRemoteAction setTitle:inactive ? @" Standby" : connected ? @" Connected" : @" Disconnected" forState:UIControlStateNormal];
    UIColor *color=inactive ? UIColor.systemOrangeColor : connected ? UIColor.systemGreenColor : UIColor.systemRedColor;
    [IPSRemoteAction setTitleColor:color forState:UIControlStateNormal]; IPSRemoteAction.tintColor=color;
    IPSRemoteAction.accessibilityLabel=inactive ? @"IPS Standby. Server bereikbaar, ander device actief" : connected ? @"IPS Connected. Open verbindingsinformatie" : @"IPS Disconnected. Open verbindingsinformatie";
}
static void IPSLayoutRemoteAction(UIView *row) {
    if (!IPSActionRows) IPSActionRows=[NSHashTable weakObjectsHashTable];
    [IPSActionRows addObject:row];
    UIButton *remote=objc_getAssociatedObject(row,IPSActionKey);
    NSArray *prior=objc_getAssociatedObject(row,IPSHiddenKey);
    if (!IPSEnabled()) {
        for (NSDictionary *entry in prior) { UIView *v=entry[@"view"]; v.hidden=[entry[@"hidden"] boolValue]; }
        remote.hidden=YES; objc_setAssociatedObject(row,IPSHiddenKey,nil,OBJC_ASSOCIATION_RETAIN_NONATOMIC); return;
    }
    NSMutableArray *targets=NSMutableArray.array;
    id download=IPSObject(row,@"downloadButton");
    if ([download isKindOfClass:UIView.class] && [download isDescendantOfView:row]) [targets addObject:download];
    NSMutableArray *pending=[row.subviews mutableCopy]; NSUInteger visited=0;
    while (pending.count && visited++<700) {
        UIView *view=pending.lastObject; [pending removeLastObject];
        if (view==remote) continue;
        NSString *label=view.accessibilityLabel ?: @"";
        if ([view isKindOfClass:UIButton.class]) label=[(UIButton *)view titleForState:UIControlStateNormal] ?: label;
        NSString *lower=label.lowercaseString;
        if (!lower.length && [view isKindOfClass:UILabel.class]) lower=[(UILabel *)view text].lowercaseString ?: @"";
        BOOL match=IPSMatch(lower,@"^(delen|share|downloaden|download)(\\b|$)");
        if (match) {
            UIView *target=view;
            while (target.superview && target.superview!=row && (target.bounds.size.height<30 || target.bounds.size.width<65)) target=target.superview;
            if (target.bounds.size.height<=90 && target.bounds.size.width<=350 && ![targets containsObject:target]) [targets addObject:target];
        } else [pending addObjectsFromArray:view.subviews];
    }
    UIView *anchor=nil;
    if ([download isKindOfClass:UIView.class] && [download isDescendantOfView:row]) anchor=download;
    if (!anchor) for (UIView *target in targets) {
        NSString *label=(target.accessibilityLabel ?: @"").lowercaseString;
        if (IPSMatch(label,@"^(downloaden|download)(\\b|$)")) {anchor=target;break;}
    }
    if (!anchor && targets.count==1) anchor=targets.firstObject;
    if (!anchor || anchor==row || [anchor isKindOfClass:UIScrollView.class] || CGRectIsEmpty(anchor.bounds)) { remote.hidden=YES; return; }
    // Keep the native slot in the scroll content. Never union coordinates from
    // different parents or move a replacement across the scrolling viewport.
    if (!remote) {
        remote=[UIButton buttonWithType:UIButtonTypeSystem];
        [remote setImage:[UIImage systemImageNamed:@"appletvremote.gen1"] forState:UIControlStateNormal];
        remote.titleLabel.font=[UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
        remote.titleLabel.adjustsFontSizeToFitWidth=YES; remote.titleLabel.minimumScaleFactor=0.5;
        remote.backgroundColor=[UIColor colorWithWhite:0.15 alpha:1]; remote.layer.cornerRadius=16;
        remote.clipsToBounds=YES;
        [remote addTarget:IPSMenu.class action:@selector(show) forControlEvents:UIControlEventTouchUpInside];
        objc_setAssociatedObject(row,IPSActionKey,remote,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    if (remote.superview!=anchor) [anchor addSubview:remote];
    objc_setAssociatedObject(anchor,IPSPinnedKey,remote,OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    remote.frame=anchor.bounds;
    remote.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
    remote.hidden=NO; [anchor bringSubviewToFront:remote];
    IPSRemoteAction=remote; IPSUpdateRemoteAction();

}
static void IPSRefreshOverlay(void) {
    IPSUpdateRemoteAction();
    for (UIView *row in IPSActionRows.allObjects) if (row.window) IPSLayoutRemoteAction(row);
    static __weak UIViewController *reservedRoot;
    static CGFloat reservedTop;
    UIWindow *window=nil;
    for (UIWindow *candidate in UIApplication.sharedApplication.windows) if (candidate.isKeyWindow) {window=candidate;break;}
    if (!window) return;
    if (!IPSOverlay || IPSOverlay.superview!=window) {
        [IPSOverlay removeFromSuperview]; IPSOverlay=UILabel.new;
        IPSOverlay.backgroundColor=[UIColor colorWithRed:0.07 green:0.12 blue:0.12 alpha:0.94];
        IPSOverlay.textColor=UIColor.whiteColor; IPSOverlay.font=[UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
        IPSOverlay.numberOfLines=2; IPSOverlay.textAlignment=NSTextAlignmentCenter;
        IPSOverlay.layer.cornerRadius=10; IPSOverlay.clipsToBounds=YES;
        IPSOverlay.userInteractionEnabled=YES;
        [IPSOverlay addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:IPSMenu.class action:@selector(show)]];
        [window addSubview:IPSOverlay];
    }
    // Keep authentication and settings unobstructed.
    UIViewController *top=IPSPresenter();
    if ([top isKindOfClass:UINavigationController.class]) top=[(UINavigationController *)top topViewController];
    NSString *name=NSStringFromClass(top.class);
    BOOL authentication=[name rangeOfString:@"auth" options:NSCaseInsensitiveSearch].location!=NSNotFound || [name rangeOfString:@"signin" options:NSCaseInsensitiveSearch].location!=NSNotFound || [name hasPrefix:@"SF"];
    IPSOverlay.hidden=YES; // Superseded by the inline remote action; ignore old saved preferences.
    UIViewController *root=window.rootViewController;
    CGFloat needed=IPSOverlay.hidden ? 0 : 54;
    if (reservedRoot!=root) {
        if (reservedRoot) {
            UIEdgeInsets old=reservedRoot.additionalSafeAreaInsets;
            old.top=MAX(0,old.top-reservedTop); reservedRoot.additionalSafeAreaInsets=old;
        }
        reservedRoot=root; reservedTop=0;
    }
    if (reservedTop!=needed) {
        UIEdgeInsets inset=root.additionalSafeAreaInsets;
        inset.top=MAX(0,inset.top-reservedTop)+needed;
        root.additionalSafeAreaInsets=inset; reservedTop=needed;
    }
    IPSOverlay.frame=CGRectMake(12,MAX(window.safeAreaInsets.top,20)+4,window.bounds.size.width-24,46);
    NSString *status=IPSFailures ? (!IPSAllowFallback ? @"Verbinding weg · pauze blijft behouden" : IPSEmergency.playing ? @"Verbinding weg · lokaal noodnummer" : IPSOfflineFallback ? @"Verbinding weg · fallbackplaylist" : @"Verbinding weg · huidige plaat blijft spelen") : ![IPSConfig[@"online"] boolValue] ? @"Liveshow · gepauzeerd" : @"Verbonden · audiomodus aangevraagd";
    if (!IPSLastContact && !IPSFailures) status=@"Verbinden…";
    IPSOverlay.text=[NSString stringWithFormat:@"IPS · %@ · %@\n%@ · tik voor instellingen",[NSURL URLWithString:IPSSetting(@"url")].host ?: @"",IPSEvent(),status];
    [window bringSubviewToFront:IPSOverlay];
}
