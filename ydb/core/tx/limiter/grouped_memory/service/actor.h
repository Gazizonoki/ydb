#pragma once
#include "counters.h"
#include "manager.h"

#include <ydb/core/base/memory_controller_iface.h>
#include <ydb/core/tx/limiter/grouped_memory/usage/config.h>
#include <ydb/core/tx/limiter/grouped_memory/usage/events.h>

#include <ydb/library/actors/core/actor.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

namespace NKikimr::NOlap::NGroupedMemoryManager {
class TManager;
class TMemoryLimiterActor: public NActors::TActorBootstrapped<TMemoryLimiterActor> {
private:
    std::shared_ptr<TManager> Manager;
    const TConfig Config;
    const TString Name;
    const std::shared_ptr<TCounters> Signals;
    const std::shared_ptr<TStageFeatures> DefaultStage;
    const NMemory::EMemoryConsumerKind ConsumerKind;

public:
    TMemoryLimiterActor(const TConfig& config, const TString& name, const std::shared_ptr<TCounters>& signals,
        const std::shared_ptr<TStageFeatures>& defaultStage, const NMemory::EMemoryConsumerKind consumerKind)
        : Config(config)
        , Name(name)
        , Signals(signals)
        , DefaultStage(defaultStage)
        , ConsumerKind(consumerKind) {
    }

    void Handle(NEvents::TEvExternal::TEvStartTask::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvFinishTask::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvUpdateTask::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvStartGroup::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvFinishGroup::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvStartProcess::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvFinishProcess::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvStartProcessScope::TPtr& ev);
    void Handle(NEvents::TEvExternal::TEvFinishProcessScope::TPtr& ev);
    void Handle(NMemory::TEvConsumerRegistered::TPtr& ev);
    void Handle(NMemory::TEvConsumerLimit::TPtr& ev);

    void Bootstrap();

    STFUNC(StateWait) {
        switch (ev->GetTypeRewrite()) {
            hFunc(NEvents::TEvExternal::TEvStartTask, Handle);
            hFunc(NEvents::TEvExternal::TEvFinishTask, Handle);
            hFunc(NEvents::TEvExternal::TEvUpdateTask, Handle);
            hFunc(NEvents::TEvExternal::TEvStartGroup, Handle);
            hFunc(NEvents::TEvExternal::TEvFinishGroup, Handle);
            hFunc(NEvents::TEvExternal::TEvStartProcess, Handle);
            hFunc(NEvents::TEvExternal::TEvFinishProcess, Handle);
            hFunc(NEvents::TEvExternal::TEvStartProcessScope, Handle);
            hFunc(NEvents::TEvExternal::TEvFinishProcessScope, Handle);
            hFunc(NMemory::TEvConsumerRegistered, Handle);
            hFunc(NMemory::TEvConsumerLimit, Handle);
            default:
                AFL_VERIFY(false)("ev_type", ev->GetTypeName());
        }
    }
};

}   // namespace NKikimr::NOlap::NGroupedMemoryManager
