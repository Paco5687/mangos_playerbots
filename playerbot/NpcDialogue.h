/*
 * NPC ambient dialogue (issue #48) — townsfolk hear player /say and reply in a
 * chat bubble.
 *
 * Design constraints, all deliberate (see issue #48 for the full gating table):
 *   - The world thread is sacred. Generate() is NEVER called on it; requests go
 *     out via std::async and the future is polled non-blocking from the tick,
 *     mirroring the proven RpgAIChatAction pattern (WaitForLines/SpeakLine).
 *   - Creatures speak through WorldObject::MonsterSay, which needs no
 *     WorldSession — so none of the bot mailbox machinery is involved.
 *   - Real players only. Bot LLM replies arrive as CMSG_MESSAGECHAT queued into
 *     the bot's own session, so they reach the same handler; if NPCs answered
 *     bots, an NPC bubble would be heard by a nearby 'ai chat' bot, which would
 *     reply, which the NPC would hear -- a loop consuming both generation slots
 *     indefinitely.
 *   - Named NPCs only: one spawn in the world AND a gossip menu. Loaded once at
 *     startup into a set, matching services/knowledge_packs.is_eligible().
 *   - Everything is bounded: range, per-creature cooldown, nearest-only, and a
 *     cap on how many generation slots NPCs may hold.
 */
#ifndef _NPC_DIALOGUE_H
#define _NPC_DIALOGUE_H

#include "Common.h"
#include "Entities/ObjectGuid.h"

#include <future>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Player;
class Creature;

class NpcDialogue
{
    public:
        static NpcDialogue& instance()
        {
            static NpcDialogue s_instance;
            return s_instance;
        }

        // Load the eligible-NPC set and the exported knowledge/bark file.
        // Safe to call again to reload (e.g. from a debug command).
        void Load();

        // World thread. Called from the chat handler when a player speaks.
        // Picks the nearest eligible listener and either barks immediately
        // (free) or dispatches a generation request (bounded).
        void OnPlayerChat(Player* player, const std::string& msg, uint32 lang);

        // World thread. Called every tick; delivers any finished replies.
        void Update();

        bool IsEnabled() const { return m_enabled; }
        size_t EligibleCount() const { return m_eligible.size(); }

    private:
        NpcDialogue() : m_enabled(false), m_loaded(false) {}
        NpcDialogue(NpcDialogue const&) = delete;
        NpcDialogue& operator=(NpcDialogue const&) = delete;

        // Per-NPC material exported by the worldservice (services/npc_export.py):
        // pre-generated tracery barks plus a compact knowledge blurb used as
        // retrieval context for the LLM.
        struct NpcLines
        {
            std::vector<std::string> greet;
            std::vector<std::string> idle;
            std::vector<std::string> deflect;
            std::string knowledge;      // compact context for the prompt
        };

        struct Pending
        {
            ObjectGuid creature;
            ObjectGuid listener;
            uint32 mapId;
            std::future<std::string> reply;
        };

        bool IsEligible(uint32 entry) const
        {
            return m_eligible.find(entry) != m_eligible.end();
        }

        // 'greet' | 'ambient' | 'question' — mirrors npc_barks.classify()
        static std::string Classify(const std::string& msg);

        Creature* FindNearestListener(Player* player, float range) const;
        bool OnCooldown(ObjectGuid guid, uint32 now) const;
        void SetCooldown(ObjectGuid guid, uint32 now);

        std::string PickLine(uint32 entry, const std::string& kind) const;
        std::string BuildPrompt(Creature* creature, Player* player,
                                const std::string& msg) const;
        void Speak(Creature* creature, const std::string& text) const;

        bool m_enabled;
        bool m_loaded;
        std::unordered_set<uint32> m_eligible;          // creature_template.entry
        std::unordered_map<uint32, NpcLines> m_lines;   // entry -> exported material
        std::unordered_map<uint64, uint32> m_cooldown;  // creature guid -> expiry
        std::vector<Pending> m_pending;                 // in-flight generations
};

#define sNpcDialogue NpcDialogue::instance()

#endif
