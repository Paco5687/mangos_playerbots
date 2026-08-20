/*
 * Durable directives (epic #61 phase 1b) — the mind↔body contract.
 *
 * The conductor used to steer personas through console chat-verbs: runtime-
 * only state that a relog wiped, a restart lobotomized, and a re-send reset
 * (the travel engine drops its destination when focus is re-issued). Every
 * major behavioral failure of 2026-08 was that seam.
 *
 * A directive is a row in characters.playerbot_directives, written by the
 * worldservice and re-read by the bot itself on a slow heartbeat. The body
 * CONFIGURES ITSELF from it — sets its quest focus list, adjusts its
 * strategy set — and only touches anything when the live state has drifted
 * from the directive, so re-reads never reset a journey in progress.
 * Relogs, restarts and factory strategy resets heal themselves by
 * construction: the next heartbeat re-reads the row and re-applies.
 *
 * Types (params):
 *   OUTING     ids=1,2,3   work exactly these quests (focus list)
 *   TURN_IN    ids=4,5     same mechanism; semantic split is for reports
 *   HOLD_COURT             station-keeping: strip the wanderlust strategies
 *   (none/row deleted)     body is free: no focus enforcement
 *
 * Scope: free alt bots only (the guild personas) — 3,600 rndbots never
 * touch the table.
 */
#ifndef _PLAYERBOT_DIRECTIVES_H
#define _PLAYERBOT_DIRECTIVES_H

#include "Common.h"

#include <map>
#include <mutex>
#include <set>
#include <string>

class PlayerbotAI;

class PlayerbotDirectives
{
    public:
        static PlayerbotDirectives& instance()
        {
            static PlayerbotDirectives s_instance;
            return s_instance;
        }

        // Called from PlayerbotAI::UpdateAIInternal. Cheap no-op until the
        // per-bot refresh interval elapses; then one PK SELECT and, only if
        // live state drifted from the directive, the minimal correction.
        void Sync(PlayerbotAI* ai);

    private:
        PlayerbotDirectives() {}

        struct Row
        {
            std::string type;
            std::string params;
            bool exists = false;
        };

        Row Fetch(uint32 guid);
        void Apply(PlayerbotAI* ai, Row const& row);
        static std::set<uint32> ParseIds(std::string const& params);

        std::mutex m_lock;
        std::map<uint32, time_t> m_nextCheck;   // guid -> next DB read
};

#define sPlayerbotDirectives PlayerbotDirectives::instance()

#endif
