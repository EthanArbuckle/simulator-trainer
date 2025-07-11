//
//  CommandRunner.h
//
//
//  Created by Ethan Arbuckle on 3/15/24.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface CommandRunner : NSObject

+ (BOOL)runCommand:(NSString *)command withArguments:(NSArray<NSString *> *)arguments cwd:(NSString * _Nullable)cwdPath environment:(NSDictionary * _Nullable)environment stdoutString:(NSString * _Nullable *_Nullable)stdoutString error:(NSError ** _Nullable)errorOut;

+ (BOOL)runCommand:(NSString *)command withArguments:(NSArray<NSString *> *)arguments stdoutString:(NSString * _Nullable * _Nullable)stdoutString error:(NSError ** _Nullable)error;

+ (BOOL)runCommand:(NSString *)command withArguments:(NSArray<NSString *> *)arguments cwd:(NSString * _Nullable)cwdPath environment:(NSDictionary * _Nullable)environment stdoutString:(NSString * _Nullable *_Nullable)stdoutString error:(NSError ** _Nullable)errorOut waitUntilExit:(BOOL)waitUntilExit;

+ (NSString *)xcrunInvokeAndWait:(NSArray<NSString *> *)argument;

@end

NS_ASSUME_NONNULL_END
