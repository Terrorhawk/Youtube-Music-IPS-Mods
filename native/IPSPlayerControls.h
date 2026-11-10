static NSString *IPSControlEpoch;
static NSMutableSet *IPSRetiredControlEpochs;
static unsigned long long IPSControlSequence;
static BOOL IPSControlHasSequence(NSDictionary *command){return [command[@"sequence"] isKindOfClass:NSNumber.class] && [command[@"sequence"] unsignedLongLongValue]>0 && [command[@"controlEpoch"] isKindOfClass:NSString.class] && [command[@"controlEpoch"] length]>0;}
static BOOL IPSControlIsStale(NSDictionary *command){
    if(!IPSControlHasSequence(command))return [command[@"expiresAt"] doubleValue]<IPSLatestControlExpiry;
    NSString *epoch=command[@"controlEpoch"];
    return [IPSRetiredControlEpochs containsObject:epoch] || ([epoch isEqual:IPSControlEpoch] && [command[@"sequence"] unsignedLongLongValue]<IPSControlSequence);
}
static void IPSControlAccepted(NSDictionary *command){
    IPSLatestControlExpiry=MAX(IPSLatestControlExpiry,[command[@"expiresAt"] doubleValue]);
    if(!IPSControlHasSequence(command))return;
    if(![IPSControlEpoch isEqual:command[@"controlEpoch"]]){if(!IPSRetiredControlEpochs)IPSRetiredControlEpochs=NSMutableSet.new;if(IPSControlEpoch)[IPSRetiredControlEpochs addObject:IPSControlEpoch];IPSControlEpoch=command[@"controlEpoch"];IPSControlSequence=0;}
    IPSControlSequence=MAX(IPSControlSequence,[command[@"sequence"] unsignedLongLongValue]);
}
static void IPSResetControlOrder(void){IPSControlEpoch=nil;IPSControlSequence=0;IPSRetiredControlEpochs=nil;IPSLatestControlExpiry=0;}
// Device controls share authoritative pause/resume intent with the server.
static NSDictionary *IPSUserTransportPending;
static BOOL IPSTransportBusy,IPSLocalUserPaused,IPSControlsRestored;
static NSString *IPSTransportKey(void){return [NSString stringWithFormat:@"IPS.transport.%@.%@",IPSSetting(@"url"),IPSEvent()];}
static void IPSRestoreControlState(void){if(!IPSControlsRestored){IPSLocalUserPaused=[IPSD() boolForKey:[IPSTransportKey() stringByAppendingString:@".paused"]];IPSControlsRestored=YES;}}
static void IPSSetLocalPause(BOOL paused){IPSControlsRestored=YES;IPSLocalUserPaused=paused;[IPSD() setBool:paused forKey:[IPSTransportKey() stringByAppendingString:@".paused"]];}
static BOOL IPSLocalPlaybackPaused(void){IPSRestoreControlState();return IPSEnabled() && IPSLocalUserPaused;}
static void IPSStoreTransport(void){if(IPSUserTransportPending)[IPSD() setObject:[NSJSONSerialization dataWithJSONObject:IPSUserTransportPending options:0 error:NULL] forKey:IPSTransportKey()];else[IPSD() removeObjectForKey:IPSTransportKey()];}
static BOOL IPSUserLoadBlocked(NSString *action){return [IPSUserTransportPending[@"action"] isEqual:@"pause"] && ( [action isEqual:@"load"] || [action isEqual:@"resume"] );}
static void IPSUserCommandWillApply(NSString *action){if([action isEqual:@"pause"] || [action isEqual:@"stop"])IPSSetLocalPause(YES);else if([action isEqual:@"load"] || [action isEqual:@"resume"])IPSSetLocalPause(NO);}
static void IPSResetUserTransport(BOOL clear){IPSTransportBusy=NO;if(clear){IPSUserTransportPending=nil;IPSSetLocalPause(NO);IPSStoreTransport();}}
static BOOL IPSSendTransportIfPending(void){
    IPSRestoreControlState();
    if(!IPSUserTransportPending){id raw=[IPSD() objectForKey:IPSTransportKey()];id saved=[raw isKindOfClass:NSData.class]?[NSJSONSerialization JSONObjectWithData:raw options:0 error:NULL]:raw;if([saved isKindOfClass:NSDictionary.class]){IPSUserTransportPending=saved;IPSLocalUserPaused=[IPSD() boolForKey:[IPSTransportKey() stringByAppendingString:@".paused"]];if(IPSLocalUserPaused)IPSAllowFallback=NO;}}
    if(!IPSUserTransportPending)return NO;
    if(IPSTransportBusy)return YES;
    if(!IPSEnabled() || IPSInactive)return NO;
    IPSTransportBusy=YES;IPSBusy=YES;
    NSDictionary *intent=IPSUserTransportPending;
    IPSRequest(@"transport",intent,^(NSDictionary *reply){
        IPSTransportBusy=NO;IPSBusy=NO;
        if(IPSHTTPStatus==200 && [reply[@"command"] isKindOfClass:NSDictionary.class]){
            NSDictionary *command=reply[@"command"];
            if([command[@"expiresAt"] doubleValue]<=NSDate.date.timeIntervalSince1970*1000+IPSClockOffset){
                if([[[IPSD() dictionaryForKey:IPSJournalKey()] objectForKey:command[@"id"]][@"result"] isEqual:@"submitted"]){IPSUserTransportPending=nil;IPSStoreTransport();IPSNextPoll=0;return;}
                NSMutableDictionary *retry=[intent mutableCopy];retry[@"id"]=NSUUID.UUID.UUIDString;retry[@"videoId"]=IPSVideo();retry[@"playback"]=IPSStatus();retry[@"loadCommandId"]=IPSActiveLoadCommand ?: @"";IPSUserTransportPending=retry;IPSStoreTransport();IPSNextPoll=0;return;
            }
            if([IPSUserTransportPending[@"id"] isEqual:intent[@"id"]]){IPSUserTransportPending=nil;IPSStoreTransport();}
            IPSFailures=0;IPSNextPoll=0;IPSCommand(command);IPSRefreshSurface();
        }else if(IPSHTTPStatus>=400 && IPSHTTPStatus<500 && IPSHTTPStatus!=408 && IPSHTTPStatus!=429){
            IPSUserTransportPending=nil;IPSStoreTransport();IPSNextPoll=0;IPSCrossInfo=reply[@"error"] ?: [NSString stringWithFormat:@"Bediening geweigerd (%ld); controleer de serverversie.",(long)IPSHTTPStatus];
        }else{
            IPSFailures=MIN(IPSFailures+1,12);IPSNextPoll=NSDate.date.timeIntervalSince1970+MIN(60,4*pow(2,MIN(IPSFailures-1,4)));
            IPSCrossInfo=reply[@"error"] ?: @"Bediening wacht op verbinding met de server.";
        }
    });return YES;
}
static void IPSRequestTransport(NSString *action){
    if(!IPSEnabled() || IPSInactive || IPSUserTransportPending)return;
    NSDictionary *proof=IPSStatus();
    IPSUserTransportPending=@{@"id":NSUUID.UUID.UUIDString,@"action":action,@"videoId":IPSVideo(),@"loadCommandId":IPSActiveLoadCommand ?: @"",@"playback":proof};
    IPSStoreTransport();
    if([action isEqual:@"pause"]){
        IPSSetLocalPause(YES);IPSAllowFallback=NO;[IPSEmergency stop];
        [IPSQueueTimer invalidate];IPSQueueTimer=nil;
        BOOL auxiliary=IPSAuxiliaryEvent;IPSAuxiliaryEvent=YES;
        IPSInvoke(IPSPlayer,@"pause");if(IPSOutgoing!=IPSPlayer)IPSInvoke(IPSOutgoing,@"pause");if(IPSProbeSecondary!=IPSPlayer)IPSInvoke(IPSProbeSecondary,@"pause");
        IPSAuxiliaryEvent=auxiliary;
        NSMutableDictionary *cache=[IPSConfig mutableCopy];if(cache){cache[@"allowFallback"]=@NO;[IPSD() setObject:cache forKey:IPSConfigKey()];}
        IPSRefreshSurface();
    }
    IPSNextPoll=0;IPSSendTransportIfPending();
}
static void IPSRequestToggle(void){
    // MLHAM's rate may stay at the requested speed while paused.
    // An acknowledged/local pause must therefore win over that rate.
    if(IPSLocalPlaybackPaused()){IPSRequestTransport(@"resume");return;}
    BOOL playing=NO;if(!IPSActualPlaying(&playing))playing=IPSPlaying;
    IPSRequestTransport(playing?@"pause":@"resume");
}
