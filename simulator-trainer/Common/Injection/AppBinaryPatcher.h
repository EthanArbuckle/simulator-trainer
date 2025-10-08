//
//  AppBinaryPatcher.h
//  simulator-trainer
//
//  Created by Ethan Arbuckle on 4/29/25.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface AppBinaryPatcher : NSObject

+ (void)injectDylib:(NSString *)dylibPath intoBinary:(NSString *)binaryPath usingOptoolAtPath:(NSString *)optoolPath completion:(void (^ _Nullable)(BOOL success, NSError * _Nullable error))completion;
+ (void)codesignItemAtPath:(NSString *)path completion:(void (^)(BOOL, NSError * _Nullable))completion;
+ (void)thinBinaryAtPath:(NSString *)binaryPath;
+ (BOOL)isBinaryArm64SimulatorCompatible:(NSString *)binaryPath;

@end

NS_ASSUME_NONNULL_END
