//
//  PackageInstallationService.m
//  simulator-trainer
//
//  Created by m1book on 5/23/25.
//

#import "PackageInstallationService.h"
#import "platform_changer.h"
#import "AppBinaryPatcher.h"
#import "CommandRunner.h"
#import "tmpfs_overlay.h"
#import <objc/runtime.h>
#import <objc/message.h>

@implementation PackageInstallationService

- (NSArray *)_minimalOverlayDirsForFilesToCopy:(NSDictionary<NSString *, NSString *> *)filesToCopy simRuntimeRoot:(NSString *)simRuntimeRoot {
    NSMutableSet *parentDirs = [[NSMutableSet alloc] init];
    for (NSString *destPath in filesToCopy.allValues) {
        if (![destPath hasPrefix:simRuntimeRoot]) {
            continue;
        }

        NSString *relativePath = [destPath substringFromIndex:simRuntimeRoot.length];
        if ([relativePath hasPrefix:@"/"]) {
            relativePath = [relativePath substringFromIndex:1];
        }

        NSString *parentDir = [relativePath stringByDeletingLastPathComponent];
        if (parentDir.length == 0) {
            continue;
        }
        
        NSString *checkPath = [simRuntimeRoot stringByAppendingPathComponent:parentDir];
        while (![ [NSFileManager defaultManager] fileExistsAtPath:checkPath isDirectory:NULL]) {
            parentDir = [parentDir stringByDeletingLastPathComponent];
            if (parentDir.length == 0) {
                break;
            }
            
            checkPath = [simRuntimeRoot stringByAppendingPathComponent:parentDir];
        }
        
        if (parentDir.length == 0) {
            continue;
        }
        
        [parentDirs addObject:parentDir];
    }

    NSArray *sortedDirs = [[parentDirs allObjects] sortedArrayUsingSelector:@selector(compare:)];
    NSMutableArray *minimalOverlays = [[NSMutableArray alloc] init];
    for (NSString *dir in sortedDirs) {
        BOOL covered = NO;
        for (NSString *existing in minimalOverlays) {
            if ([dir hasPrefix:existing] && (dir.length == existing.length || [dir characterAtIndex:existing.length] == '/')) {
                covered = YES;
                break;
            }
        }
        
        if (!covered) {
            [minimalOverlays addObject:dir];
        }
    }

    NSMutableArray *result = [[NSMutableArray alloc] init];
    for (NSString *dir in minimalOverlays) {
        [result addObject:[simRuntimeRoot stringByAppendingPathComponent:dir]];
    }

    return result;
}

- (void)installDebFileAtPath:(NSString *)debPath toDevice:(BootedSimulatorWrapper *)device serviceConnection:(HelperConnection *)connection completion:(void (^)(NSError * _Nullable error))completion {
    if (!debPath || !device) {
        if (completion) {
            completion([NSError errorWithDomain:NSCocoaErrorDomain code:1 userInfo:@{NSLocalizedDescriptionKey: @"Invalid parameters: debPath or device is nil."}]);
        }
        
        return;
    }
        
    NSString *simRuntimeRoot = device.runtimeRoot;
    if (!simRuntimeRoot) {
        completion([NSError errorWithDomain:NSCocoaErrorDomain code:98 userInfo:@{NSLocalizedDescriptionKey: @"Simulator runtime root path is nil."}]);
        return;
    }
    
    NSString *tempExtractDir = [NSTemporaryDirectory() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
    NSString *dataTarExtractDir = [tempExtractDir stringByAppendingPathComponent:@"data_payload"];
    NSError * __block operationError = nil;
    
    if (![[NSFileManager defaultManager] createDirectoryAtPath:tempExtractDir withIntermediateDirectories:YES attributes:nil error:&operationError]) {
        if (completion) {
            completion(operationError);
        }
        
        return;
    }
    
    void (^cleanupBlock)(void) = ^{
        [[NSFileManager defaultManager] removeItemAtPath:tempExtractDir error:nil];
    };
    
    NSString *debFileName = [debPath lastPathComponent];
    NSString *copiedDebPath = [tempExtractDir stringByAppendingPathComponent:debFileName];
    if (![[NSFileManager defaultManager] copyItemAtPath:debPath toPath:copiedDebPath error:&operationError]) {
        cleanupBlock();
        if (completion) {
            completion(operationError);
        }

        return;
    }
    
    if (![CommandRunner runCommand:@"/usr/bin/ar" withArguments:@[@"-x", copiedDebPath] cwd:tempExtractDir environment:nil stdoutString:nil error:&operationError]) {
        cleanupBlock();
        if (completion) {
            completion(operationError);
        }
        
        return;
    }

    NSString *dataTarName = nil;
    NSArray *possibleDataTarNames = @[@"data.tar.gz", @"data.tar.xz", @"data.tar.zst", @"data.tar.bz2", @"data.tar,", @"data.tar.lzma"];
    for (NSString *name in possibleDataTarNames) {
        if ([[NSFileManager defaultManager] fileExistsAtPath:[tempExtractDir stringByAppendingPathComponent:name]]) {
            dataTarName = name;
            break;
        }
    }
    
    if (!dataTarName) {
        cleanupBlock();
        if (completion) {
            completion([NSError errorWithDomain:NSCocoaErrorDomain code:101 userInfo:@{NSLocalizedDescriptionKey: @"No data.tar found in the deb package"}]);
        }
        
        return;
    }
    
    NSString *dataTarPath = [tempExtractDir stringByAppendingPathComponent:dataTarName];
    if (![[NSFileManager defaultManager] createDirectoryAtPath:dataTarExtractDir withIntermediateDirectories:YES attributes:nil error:&operationError]) {
        cleanupBlock();
        if (completion) {
            completion(operationError);
        }
        
        return;
    }
    
    if (![CommandRunner runCommand:@"/usr/bin/tar" withArguments:@[@"-xf", dataTarPath, @"-C", dataTarExtractDir] stdoutString:nil error:&operationError]) {
        cleanupBlock();
        if (completion) {
            completion(operationError);
        }
        return;
    }
    
    NSMutableDictionary *filesToCopy = [[NSMutableDictionary alloc] init];
    
    NSDirectoryEnumerator *dirEnumerator = [[NSFileManager defaultManager] enumeratorAtPath:dataTarExtractDir];
    NSString *fileRelativeInDataTar;
    while ((fileRelativeInDataTar = [dirEnumerator nextObject])) {
        NSString *sourcePath = [dataTarExtractDir stringByAppendingPathComponent:fileRelativeInDataTar];
        
        BOOL isDir;
        if ([[NSFileManager defaultManager] fileExistsAtPath:sourcePath isDirectory:&isDir] && !isDir) {
            NSString *cleanedRelativePath = [fileRelativeInDataTar copy];
            if ([cleanedRelativePath hasPrefix:@"./"]) {
                cleanedRelativePath = [cleanedRelativePath substringFromIndex:2];
            }

            filesToCopy[sourcePath] = [simRuntimeRoot stringByAppendingPathComponent:cleanedRelativePath];
        }
    }
    
    NSArray *expectedOverlayRoots = [self _minimalOverlayDirsForFilesToCopy:filesToCopy simRuntimeRoot:simRuntimeRoot];
    NSMutableArray *directoriesToOverlay = [[NSMutableArray alloc] init];
    for (NSString *overlayRoot in expectedOverlayRoots) {
        if (!is_tmpfs_mount(overlayRoot.UTF8String)) {
            [directoriesToOverlay addObject:overlayRoot];
        }
    }
    
    if (directoriesToOverlay.count > 0) {
        __block BOOL mountSuccess = YES;
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [connection mountTmpfsOverlaysAtPaths:directoriesToOverlay completion:^(NSError * _Nullable error) {
            
            if (error) {
                NSLog(@"Failed to mount tmpfs overlays: %@", error);
                mountSuccess = NO;
            }
            
            dispatch_semaphore_signal(sem);
        }];
        
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(30 * NSEC_PER_SEC))) != 0) {
            NSLog(@"Timeout waiting for tmpfs overlays to mount");
            mountSuccess = NO;
        }
        
        if (!mountSuccess) {
            cleanupBlock();
            if (completion) {
                completion([NSError errorWithDomain:NSCocoaErrorDomain code:102 userInfo:@{NSLocalizedDescriptionKey: @"Failed to overlay read-only dirs that the deb package needs"}]);
            }
            
            return;
        }
        
        NSLog(@"Tmpfs overlays mounted successfully");
    }
                
    
    for (NSString *sourcePath in filesToCopy) {
        NSString *destinationPath = filesToCopy[sourcePath];
        NSString *destinationParentDir = [destinationPath stringByDeletingLastPathComponent];
                
        if (![[NSFileManager defaultManager] fileExistsAtPath:destinationParentDir]) {
            if (![[NSFileManager defaultManager] createDirectoryAtPath:destinationParentDir withIntermediateDirectories:YES attributes:nil error:&operationError]) {
                cleanupBlock();
                if (completion) {
                    completion(operationError);
                }
                
                return;
            }
        }
        
        if ([[NSFileManager defaultManager] fileExistsAtPath:destinationPath]) {
            [[NSFileManager defaultManager] removeItemAtPath:destinationPath error:NULL];
        }
        
        if (![[NSFileManager defaultManager] copyItemAtPath:sourcePath toPath:destinationPath error:&operationError]) {
            NSLog(@"  copy error: %@", operationError);
            cleanupBlock();
            if (completion) {
                completion(operationError);
            }
            return;
        }
        
        if ([destinationPath.pathExtension isEqualToString:@"dylib"] && ![AppBinaryPatcher isBinaryArm64SimulatorCompatible:destinationPath]) {
            // Convert to simulator platform and then codesign
            [AppBinaryPatcher thinBinaryAtPath:destinationPath];
            convertPlatformToSimulator(destinationPath.UTF8String);
            
            [AppBinaryPatcher codesignItemAtPath:destinationPath completion:^(BOOL success, NSError *error) {
                if (!success) {
                    NSLog(@"Failed to codesign item at path: %@", error);
                }
            }];
        }
    }

    cleanupBlock();

    [device respring];

    if (completion) {
        completion(nil);
    }
}

- (void)installAppBundleAtPath:(NSString *)appPath toDevice:(BootedSimulatorWrapper *)device completion:(void (^)(NSError * _Nullable error))completion {
    NSString *appFileNameWithoutExtension = [[appPath lastPathComponent] stringByDeletingPathExtension];
    NSString *randomizedName = [NSString stringWithFormat:@"%@-%@.%@", appFileNameWithoutExtension, [[NSUUID UUID] UUIDString], [appPath pathExtension]];
    NSString *tmpAppPath = [NSTemporaryDirectory() stringByAppendingPathComponent:randomizedName];
    NSError *copyError = nil;
    if (![[NSFileManager defaultManager] copyItemAtPath:appPath toPath:tmpAppPath error:&copyError]) {
        NSLog(@"Failed to copy app bundle to temporary location: %@", copyError);
        if (completion) {
            completion(copyError);
        }
        
        return;
    }
    
    convertPlatformToSimulator(tmpAppPath.UTF8String);
    
    NSError *installError = nil;
    NSURL *tmpAppUrl = [NSURL fileURLWithPath:tmpAppPath];
    // [device.coreSimDevice installApplication:tmpAppUrl withOptions:nil error:&installError]
    ((void (*)(id, SEL, NSURL *, NSDictionary *, NSError **))objc_msgSend)(device.coreSimDevice, sel_registerName("installApplication:withOptions:error:"), tmpAppUrl, nil, &installError);
    
    if (installError) {
        NSLog(@"Failed to install app bundle: %@", installError);
    }
    
    if (completion) {
        completion(installError);
    }
    
    [[NSFileManager defaultManager] removeItemAtPath:tmpAppPath error:nil];
}

@end
