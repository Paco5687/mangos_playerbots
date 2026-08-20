
#include "playerbot/playerbot.h"
#include "playerbot/PlayerbotAIConfig.h"
#include "PlayerbotDirectives.h"

#include "playerbot/strategy/values/TravelValues.h"
#include "playerbot/TravelMgr.h"

#include "Database/DatabaseEnv.h"

#include <cstdlib>
#include <sstream>

using namespace ai;

static const time_t DIRECTIVE_REFRESH_S = 30;

std::set<uint32> PlayerbotDirectives::ParseIds(std::string const& params)
{
    // params: "ids=1,2,3" (bare "1,2,3" tolerated)
    std::set<uint32> out;
    std::string s = params;
    size_t eq = s.find("ids=");
    if (eq != std::string::npos)
        s = s.substr(eq + 4);
    std::stringstream ss(s);
    std::string piece;
    while (std::getline(ss, piece, ','))
    {
        uint32 id = uint32(atoi(piece.c_str()));
        if (id)
            out.insert(id);
    }
    return out;
}

PlayerbotDirectives::Row PlayerbotDirectives::Fetch(uint32 guid)
{
    Row row;
    auto result = CharacterDatabase.PQuery(
        "SELECT `type`, `params` FROM `playerbot_directives` WHERE `guid` = '%u'", guid);
    if (result)
    {
        Field* fields = result->Fetch();
        row.type = fields[0].GetCppString();
        row.params = fields[1].GetCppString();
        row.exists = true;
    }
    return row;
}

void PlayerbotDirectives::Sync(PlayerbotAI* ai)
{
    Player* bot = ai->GetBot();
    if (!bot || !bot->IsInWorld())
        return;
    // guild personas only; the rndbot swarm never queries the table
    if (!sPlayerbotAIConfig.IsFreeAltBot(bot))
        return;

    uint32 guid = bot->GetGUIDLow();
    time_t now = time(nullptr);
    {
        std::lock_guard<std::mutex> lock(m_lock);
        time_t& next = m_nextCheck[guid];
        if (now < next)
            return;
        next = now + DIRECTIVE_REFRESH_S;
    }

    Apply(ai, Fetch(guid));
}

void PlayerbotDirectives::Apply(PlayerbotAI* ai, Row const& row)
{
    if (!row.exists)
        return;                          // no directive: the body is free

    if (row.type == "OUTING" || row.type == "TURN_IN")
    {
        std::set<uint32> want = ParseIds(row.params);
        if (want.empty())
            return;

        // drift-only correction: equal sets mean a journey in progress is
        // never reset - this is what the console re-send could not do
        auto* focusValue = ai->GetAiObjectContext()->GetValue<focusQuestTravelList>("focus travel target");
        if (focusValue->Get() != want)
        {
            focusValue->Set(want);
            if (TravelTarget* target = ai->GetAiObjectContext()->GetValue<TravelTarget*>("travel target")->Get())
                target->SetExpireIn(1000);
        }

        // the strategies the focus machinery needs, healed if a relog's
        // factory reset stripped them; only missing ones are touched
        std::ostringstream delta;
        for (char const* required : { "quest", "grind", "travel", "rpg quest" })
            if (!ai->HasStrategy(required, BotState::BOT_STATE_NON_COMBAT))
                delta << (delta.tellp() > 0 ? "," : "") << "+" << required;
        if (delta.tellp() > 0)
            ai->ChangeStrategy(delta.str(), BotState::BOT_STATE_NON_COMBAT);
        return;
    }

    if (row.type == "HOLD_COURT")
    {
        // station-keeping: the wanderlust strategies stay off. This retires
        // the roster warden's five-minute console re-assert for Wrenna.
        std::ostringstream delta;
        for (char const* banned : { "travel", "wander", "grind", "quest", "rpg explore" })
            if (ai->HasStrategy(banned, BotState::BOT_STATE_NON_COMBAT))
                delta << (delta.tellp() > 0 ? "," : "") << "-" << banned;
        if (delta.tellp() > 0)
            ai->ChangeStrategy(delta.str(), BotState::BOT_STATE_NON_COMBAT);

        auto* focusValue = ai->GetAiObjectContext()->GetValue<focusQuestTravelList>("focus travel target");
        if (!focusValue->Get().empty())
            focusValue->Set({});
        return;
    }
    // unknown type: ignore quietly - forward compatibility with later phases
}
