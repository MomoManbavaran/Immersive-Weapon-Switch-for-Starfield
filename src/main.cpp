#include "pch.h"
#include <MinHook.h>

namespace
{
    constexpr int kSheatheDelayMs = 850;

    std::atomic<bool>     g_busy{ false };
    RE::TESForm* g_lastWeapon = nullptr;

    using EquipObject_t = bool (*)(RE::ActorEquipManager*, RE::Actor*, const RE::BGSObjectInstance&, const RE::BGSEquipSlot*, bool, bool, bool, bool, bool);

    EquipObject_t g_originalEquipObject = nullptr;

    void RequestSheathe(RE::Actor* a_actor)
    {
        if (a_actor) 
        {
            a_actor->DrawWeaponMagicHands(false);
        }
    }

    void RequestDraw(RE::Actor* a_actor)
    {
        if (a_actor) 
        {
            a_actor->DrawWeaponMagicHands(true);
        }
    }

    bool IsWeaponForm(RE::TESForm* a_form)
    {
        if (!a_form) 
        {
            return false;
        }

        if (a_form->As<RE::TESObjectWEAP>()) 
        {
            return true;
        }

        // Fallback.
        if (auto* bound = a_form->As<RE::TESBoundObject>()) 
        {
            if (bound->IsWeapon()) 
            {
                return true;
            }
        }

        return false;
    }

    struct PendingEquip
    {
        RE::ActorEquipManager* manager{};
        RE::Actor* actor{};
        RE::BGSObjectInstance   object{ nullptr, nullptr };
        const RE::BGSEquipSlot* slot{};
        bool queueEquip{};
        bool forceEquip{};
        bool playSounds{};
        bool applyNow{};
        bool locked{};
    };

    void FinishPendingEquip(PendingEquip pending)
    {
        if (!g_originalEquipObject || !pending.manager || !pending.actor) 
        {
            g_busy = false;
            return;
        }

        // REX::INFO("Holstering finished, applying delayed equip");

        g_originalEquipObject(
            pending.manager,
            pending.actor,
            pending.object,
            pending.slot,
            pending.queueEquip,
            pending.forceEquip,
            pending.playSounds,
            pending.applyNow,
            pending.locked);

        RequestDraw(pending.actor);
        g_busy = false;
    }

    void ScheduleHolsterThenEquip(PendingEquip pending)
    {
        // REX::INFO("Hard intercept: holster first");

        RequestSheathe(pending.actor);

        std::thread([pending = std::move(pending)]() mutable 
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSheatheDelayMs));

            auto run = [pending = std::move(pending)]() mutable 
            {
                FinishPendingEquip(std::move(pending));
            };

            if (auto* tasks = SFSE::GetTaskInterface()) 
            {
                tasks->AddTask(std::move(run));
            }
            else 
            {
                run();
            }

        }).detach();
    }

    bool hkEquipObject(
        RE::ActorEquipManager* a_this,
        RE::Actor* a_actor,
        const RE::BGSObjectInstance& a_object,
        const RE::BGSEquipSlot* a_slot,
        bool a_queueEquip,
        bool a_forceEquip,
        bool a_playSounds,
        bool a_applyNow,
        bool a_locked)
    {
        auto callOriginal = [&]() 
        {
            return g_originalEquipObject(a_this, a_actor, a_object, a_slot, a_queueEquip, a_forceEquip, a_playSounds, a_applyNow, a_locked);
        };

        if (!a_actor || !a_actor->IsPlayerRef()) 
        {
            return callOriginal();
        }

        RE::TESForm* form = a_object.object;

        /*
        REX::INFO(
            "EquipObject player | form={} | weapon={} | last={} | busy={}",
            form && form->GetFormEditorID() ? form->GetFormEditorID() : "?",
            IsWeaponForm(form),
            g_lastWeapon && g_lastWeapon->GetFormEditorID() ? g_lastWeapon->GetFormEditorID() : "(none)",
            g_busy.load());
        */

        if (!IsWeaponForm(form)) 
        {
            return callOriginal();
        }

        if (g_busy.load()) 
        {
            // REX::INFO("EquipObject swallowed (busy, delayed equip pending)");
            return true;
        }

        if (!g_lastWeapon || g_lastWeapon == form) {
            auto result = callOriginal();
            g_lastWeapon = form;
            return result;
        }

        // Different weapon = hard intercept.
        /*
        REX::INFO(
            "Intercept switch {} -> {}",
            g_lastWeapon->GetFormEditorID(),
            form->GetFormEditorID());
        */

        if (g_busy.exchange(true)) 
        {
            auto result = callOriginal();
            g_lastWeapon = form;
            return result;
        }

        PendingEquip pending
        {
            a_this,
            a_actor,
            a_object,
            a_slot,
            a_queueEquip,
            a_forceEquip,
            a_playSounds,
            a_applyNow,
            a_locked
        };

        g_lastWeapon = form;  // Target weapon after delay.
        ScheduleHolsterThenEquip(std::move(pending));
        return true;  // Block immediate equip.
    }

    // Keep last-weapon in sync for paths that aren't intercepted.
    class PlayerWeaponEquipSink :
        public RE::BSTEventSink<RE::ActorItemEquipped::Event>
    {
    public:
        RE::BSEventNotifyControl ProcessEvent(
            const RE::ActorItemEquipped::Event& a_event,
            RE::BSTEventSource<RE::ActorItemEquipped::Event>*) override
        {
            if (a_event.actor && a_event.actor->IsPlayerRef() &&
                a_event.item && IsWeaponForm(a_event.item)) 
            {
                g_lastWeapon = a_event.item;
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };

    PlayerWeaponEquipSink g_equipSink;

    bool InstallEquipHook()
    {
        if (MH_Initialize() != MH_OK) 
        {
            REX::INFO("MH_Initialize failed");
            return false;
        }

        REL::Relocation<std::uintptr_t> target{ RE::ID::ActorEquipManager::EquipObject };

        const auto status = MH_CreateHook(
            reinterpret_cast<void*>(target.address()),
            reinterpret_cast<void*>(&hkEquipObject),
            reinterpret_cast<void**>(&g_originalEquipObject));

        if (status != MH_OK) 
        {
            REX::INFO("MH_CreateHook failed ({})", static_cast<int>(status));
            return false;
        }

        if (MH_EnableHook(reinterpret_cast<void*>(target.address())) != MH_OK) 
        {
            REX::INFO("MH_EnableHook failed");
            return false;
        }

        REX::INFO("EquipObject hard-hooked via MinHook at {:X}", target.address());
        return true;
    }

    void OnDataLoaded()
    {
        if (auto* src = RE::ActorItemEquipped::Event::GetEventSource()) 
        {
            src->RegisterSink(&g_equipSink);
            REX::INFO("Registered ActorItemEquipped sink");
        }

        if (!InstallEquipHook()) {
            REX::INFO("Equip hook failed! Mod will not intercept switches");
        }
    }

    void MessageHandler(SFSE::MessagingInterface::Message* a_msg)
    {
        if (a_msg->type == SFSE::MessagingInterface::kPostDataLoad) 
        {
            OnDataLoaded();
        }
    }
}

SFSE_PLUGIN_PRELOAD(const SFSE::PreLoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    REX::INFO("ImmersiveWeaponSwitching PreLoad");
    return true;
}

SFSE_PLUGIN_LOAD(const SFSE::LoadInterface* a_sfse)
{
    SFSE::Init(a_sfse);
    REX::INFO("ImmersiveWeaponSwitching Load");

    if (auto* messaging = SFSE::GetMessagingInterface()) 
    {
        messaging->RegisterListener(MessageHandler);
    }
    return true;
}