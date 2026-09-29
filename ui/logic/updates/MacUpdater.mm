#include "logic/updates/MacUpdater.hpp"

#include <QPointer>
#include <QTimer>
#import <Sparkle/Sparkle.h>

@interface OpenSCPUpdaterDelegate : NSObject <SPUUpdaterDelegate>
@property(nonatomic, copy) BOOL (^canRestart)(void);
@property(nonatomic, copy) void (^prepareRestart)(void);
@property(nonatomic, copy) void (^pendingInstall)(void);
@property(nonatomic, copy) void (^cycleFinished)(void);
@property(nonatomic, copy) void (^waitForIdle)(void);
@end

@implementation OpenSCPUpdaterDelegate
- (BOOL)updater:(SPUUpdater *)updater
    shouldPostponeRelaunchForUpdate:(SUAppcastItem *)item
                 untilInvokingBlock:(void (^)(void))installHandler {
    (void)updater;
    (void)item;
    if (self.canRestart())
        return NO;
    self.pendingInstall = installHandler;
    self.waitForIdle();
    return YES;
}
- (BOOL)updaterShouldRelaunchApplication:(SPUUpdater *)updater {
    (void)updater;
    return self.canRestart();
}
- (void)updaterWillRelaunchApplication:(SPUUpdater *)updater {
    (void)updater;
    self.prepareRestart();
}
- (void)updater:(SPUUpdater *)updater
    didFinishUpdateCycleForUpdateCheck:(SPUUpdateCheck)check
                                 error:(NSError *)error {
    (void)updater;
    (void)check;
    (void)error;
    self.pendingInstall = nil;
    self.cycleFinished();
}
@end

namespace openscpui {
struct MacUpdater::Impl {
    OpenSCPUpdaterDelegate *delegate;
    SPUStandardUpdaterController *controller;
    bool started = false;
};
MacUpdater::MacUpdater(std::function<bool()> canRestart,
                       std::function<void()> prepareRestart, QObject *parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
    impl_->delegate = [[OpenSCPUpdaterDelegate alloc] init];
    const QPointer<MacUpdater> alive(this);
    impl_->delegate.canRestart = ^BOOL {
      return alive && canRestart();
    };
    impl_->delegate.prepareRestart = ^{
      if (alive)
          prepareRestart();
    };
    impl_->delegate.cycleFinished = ^{
      if (alive)
          emit alive->finished();
    };
    impl_->controller = [[SPUStandardUpdaterController alloc]
        initWithStartingUpdater:NO
                updaterDelegate:impl_->delegate
             userDriverDelegate:nil];
    // Shared opt-in checks are owned by UpdateController. Sparkle installs only
    // after the user's explicit confirmation in its standard UI.
    impl_->controller.updater.automaticallyChecksForUpdates = NO;
    impl_->controller.updater.automaticallyDownloadsUpdates = NO;
    impl_->controller.updater.sendsSystemProfile = NO;
    NSError *error = nil;
    impl_->started = [impl_->controller.updater startUpdater:&error];
    auto *poll = new QTimer(this);
    impl_->delegate.waitForIdle = ^{
      if (alive)
          poll->start(1000);
    };
    connect(this, &MacUpdater::finished, poll, &QTimer::stop);
    connect(poll, &QTimer::timeout, this, [this, poll] {
        auto delegate = impl_->delegate;
        if (delegate.pendingInstall && delegate.canRestart()) {
            auto install = delegate.pendingInstall;
            delegate.pendingInstall = nil;
            poll->stop();
            install();
        }
    });
}
MacUpdater::~MacUpdater() { impl_->delegate.pendingInstall = nil; }
bool MacUpdater::available() const { return impl_->started; }
bool MacUpdater::check() {
    if (!available() || !impl_->controller.updater.canCheckForUpdates)
        return false;
    [impl_->controller checkForUpdates:nil];
    return true;
}
} // namespace openscpui
