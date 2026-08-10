#include "NpcDialogue.h"

#include "PlayerbotAIConfig.h"
#include "PlayerbotLLMInterface.h"

#include "Database/DatabaseEnv.h"
#include "Entities/Creature.h"
#include "Entities/Player.h"
#include "Grids/CellImpl.h"
#include "Grids/GridNotifiers.h"
#include "Grids/GridNotifiersImpl.h"
#include "Log/Log.h"
#include "Maps/Map.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

// ---------------------------------------------------------------- loading

void NpcDialogue::Load()
{
    m_eligible.clear();
    m_lines.clear();

    m_enabled = sPlayerbotAIConfig.npcDialogueEnabled;
    if (!m_enabled)
    {
        sLog.outString("NpcDialogue: disabled by config");
        m_loaded = true;
        return;
    }

    // Named NPCs only: exactly one spawn in the world AND a gossip menu.
    // Both conditions matter -- a lone critter has one spawn but no gossip,
    // a generic guard has gossip but many spawns. Mirrors the Python rule in
    // services/knowledge_packs.is_eligible().
    auto result = WorldDatabase.Query(
        "SELECT ct.entry FROM creature_template ct "
        "WHERE ct.NpcFlags != 0 "   // any job: gossip, vendor, trainer, innkeeper...
        "AND (SELECT COUNT(*) FROM creature WHERE id = ct.entry) = 1");
    if (result)
    {
        do
        {
            m_eligible.insert((*result)[0].GetUInt32());
        } while (result->NextRow());
    }

    // Exported material: one record per NPC, pipe-delimited, produced by the
    // worldservice. Absent file is fine -- NPCs then rely on the LLM alone.
    //   entry|greet1;;greet2|idle1;;idle2|deflect1;;deflect2|knowledge blurb
    const std::string& path = sPlayerbotAIConfig.npcDialogueLinesFile;
    if (!path.empty())
    {
        std::ifstream in(path.c_str());
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#')
                continue;
            std::vector<std::string> cols;
            std::stringstream ss(line);
            std::string col;
            while (std::getline(ss, col, '|'))
                cols.push_back(col);
            if (cols.size() < 5)
                continue;

            uint32 entry = uint32(atoi(cols[0].c_str()));
            if (!entry)
                continue;

            NpcLines nl;
            auto split = [](const std::string& s, std::vector<std::string>& out)
            {
                size_t start = 0;
                while (start <= s.size())
                {
                    size_t at = s.find(";;", start);
                    std::string piece = (at == std::string::npos)
                        ? s.substr(start) : s.substr(start, at - start);
                    if (!piece.empty())
                        out.push_back(piece);
                    if (at == std::string::npos)
                        break;
                    start = at + 2;
                }
            };
            split(cols[1], nl.greet);
            split(cols[2], nl.idle);
            split(cols[3], nl.deflect);
            nl.knowledge = cols[4];
            m_lines[entry] = nl;
        }
    }

    m_loaded = true;
    sLog.outString("NpcDialogue: %zu eligible NPCs, %zu with exported lines",
                   m_eligible.size(), m_lines.size());
}

// ---------------------------------------------------------------- routing

std::string NpcDialogue::Classify(const std::string& msg)
{
    if (msg.empty())
        return "ambient";

    std::string low;
    low.reserve(msg.size());
    for (char c : msg)
        low.push_back(char(tolower((unsigned char)c)));

    // a greeting, with no question attached, is answered for free
    static const char* greetings[] = {
        "hi", "hey", "hello", "greetings", "well met", "good day",
        "good morning", "good evening", "howdy", nullptr };
    if (low.find('?') == std::string::npos)
    {
        for (int i = 0; greetings[i]; ++i)
        {
            size_t n = strlen(greetings[i]);
            if (low.compare(0, n, greetings[i]) == 0)
                return "greet";
        }
    }

    if (low.find('?') != std::string::npos)
        return "question";

    static const char* qwords[] = {
        "who ", "what ", "where ", "when ", "why ", "how ", "which ",
        "tell me", "do you know", "have you heard", "any news", nullptr };
    for (int i = 0; qwords[i]; ++i)
        if (low.find(qwords[i]) != std::string::npos)
            return "question";

    return "ambient";
}

// ---------------------------------------------------------------- helpers

bool NpcDialogue::OnCooldown(ObjectGuid guid, uint32 now) const
{
    auto it = m_cooldown.find(guid.GetRawValue());
    return it != m_cooldown.end() && it->second > now;
}

void NpcDialogue::SetCooldown(ObjectGuid guid, uint32 now)
{
    m_cooldown[guid.GetRawValue()] = now + sPlayerbotAIConfig.npcDialogueCooldown;

    // keep the map bounded; drop entries that have long expired
    if (m_cooldown.size() > 4096)
    {
        for (auto it = m_cooldown.begin(); it != m_cooldown.end(); )
            it = (it->second <= now) ? m_cooldown.erase(it) : ++it;
    }
}

Creature* NpcDialogue::FindNearestListener(Player* player, float range) const
{
    std::list<Creature*> nearby;
    MaNGOS::AnyUnitInObjectRangeCheck check(player, range);
    MaNGOS::CreatureListSearcher<MaNGOS::AnyUnitInObjectRangeCheck> searcher(nearby, check);
    Cell::VisitGridObjects(player, searcher, range);

    Creature* best = nullptr;
    float bestDist = range + 1.0f;
    for (Creature* c : nearby)
    {
        if (!c || !c->IsAlive() || c->IsInCombat())
            continue;
        if (!IsEligible(c->GetEntry()))
            continue;

        float d = player->GetDistance(c);
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return best;
}

std::string NpcDialogue::PickLine(uint32 entry, const std::string& kind) const
{
    auto it = m_lines.find(entry);
    if (it == m_lines.end())
        return "";

    const std::vector<std::string>& pool =
        (kind == "greet") ? it->second.greet :
        (kind == "deflect") ? it->second.deflect : it->second.idle;

    if (pool.empty())
        return "";
    return pool[urand(0, uint32(pool.size() - 1))];
}

std::string NpcDialogue::BuildPrompt(Creature* creature, Player* player,
                                     const std::string& msg) const
{
    std::string knowledge;
    auto it = m_lines.find(creature->GetEntry());
    if (it != m_lines.end())
        knowledge = it->second.knowledge;

    // Reuse the configured request template so endpoint/model stay in one place.
    std::string json = sPlayerbotAIConfig.llmApiJson;

    std::ostringstream pre;
    pre << sPlayerbotAIConfig.npcDialoguePrompt
        << " You are " << creature->GetName();
    if (creature->GetSubName() && *creature->GetSubName())
        pre << ", " << creature->GetSubName();
    pre << ". ";
    if (!knowledge.empty())
        pre << "What you know: " << knowledge << " ";
    pre << "If the answer is not in what you know, say plainly that you do not "
           "know and suggest who might. Answer in one or two short sentences.";

    std::string prompt = player->GetName();
    prompt += " says: ";
    prompt += msg;

    PlayerbotLLMInterface::LimitContext(json, sPlayerbotAIConfig.llmContextLength);
    // <pre prompt> / <prompt> / <context> / <post prompt> are the module's
    // existing template markers.
    auto fill = [&json](const std::string& marker, const std::string& value)
    {
        size_t at;
        std::string safe = PlayerbotLLMInterface::SanitizeForJson(value);
        while ((at = json.find(marker)) != std::string::npos)
            json.replace(at, marker.size(), safe);
    };
    fill("<pre prompt>", pre.str());
    fill("<prompt>", prompt);
    fill("<context>", "");
    fill("<post prompt>", "");
    return json;
}

void NpcDialogue::Speak(Creature* creature, const std::string& text) const
{
    if (!creature || text.empty())
        return;

    std::string line = text;
    // one bubble, never a wall of text
    const size_t kMaxBubble = 240;
    if (line.size() > kMaxBubble)
    {
        size_t cut = line.rfind('.', kMaxBubble);
        line = (cut != std::string::npos && cut > 40)
            ? line.substr(0, cut + 1) : line.substr(0, kMaxBubble);
    }
    creature->MonsterSay(line.c_str(), LANG_UNIVERSAL);
}

// ---------------------------------------------------------------- entry point

void NpcDialogue::OnPlayerChat(Player* player, const std::string& msg, uint32 /*lang*/)
{
    if (!m_loaded)
        Load();
    if (!m_enabled || !player || msg.empty())
        return;

    // Real players only. A bot's LLM reply arrives here as CMSG_MESSAGECHAT on
    // its own session; answering it would create an NPC <-> bot feedback loop
    // that consumes every generation slot. See issue #48.
#ifdef ENABLE_PLAYERBOTS
    if (player->GetPlayerbotAI())
        return;
#endif

    Creature* listener = FindNearestListener(player, sPlayerbotAIConfig.npcDialogueRange);
    if (!listener)
        return;

    uint32 now = uint32(time(nullptr));
    if (OnCooldown(listener->GetObjectGuid(), now))
        return;

    const std::string intent = Classify(msg);

    // Tier 1 -- free, instant, grounded. Greetings and passing remarks never
    // reach the model.
    if (intent != "question")
    {
        std::string line = PickLine(listener->GetEntry(), intent);
        if (!line.empty())
        {
            Speak(listener, line);
            SetCooldown(listener->GetObjectGuid(), now);
        }
        return;
    }

    // Tier 2 -- a real question. Bounded: NPCs may hold only part of the
    // generation budget so bot conversation never starves.
    if (m_pending.size() >= sPlayerbotAIConfig.npcDialogueMaxConcurrent)
    {
        std::string line = PickLine(listener->GetEntry(), "deflect");
        if (!line.empty())
        {
            Speak(listener, line);          // bark beats standing mute
            SetCooldown(listener->GetObjectGuid(), now);
        }
        return;
    }

    SetCooldown(listener->GetObjectGuid(), now);

    std::string json = BuildPrompt(listener, player, msg);
    int timeout = int(sPlayerbotAIConfig.llmGenerationTimeout);
    int maxGen = int(sPlayerbotAIConfig.llmMaxSimultaniousGenerations);

    // Resolve response patterns on the world thread; the worker gets copies
    // (same idiom as SayAction — config strings are not touched off-thread).
    std::string startPattern = sPlayerbotAIConfig.llmResponseStartPattern;
    std::string endPattern = sPlayerbotAIConfig.llmResponseEndPattern;
    std::string deletePattern = sPlayerbotAIConfig.llmResponseDeletePattern;
    std::string splitPattern = sPlayerbotAIConfig.llmResponseSplitPattern;

    Pending p;
    p.creature = listener->GetObjectGuid();
    p.listener = player->GetObjectGuid();
    p.mapId = listener->GetMapId();
    // Off the world thread. Generate() is static and self-throttling.
    p.reply = std::async(std::launch::async, [json, timeout, maxGen, startPattern, endPattern, deletePattern, splitPattern]()
    {
        std::vector<std::string> debugLines;
        std::string raw = PlayerbotLLMInterface::Generate(json, timeout, maxGen, debugLines);
        std::vector<std::string> lines = PlayerbotLLMInterface::ParseResponse(raw, startPattern, endPattern, deletePattern, splitPattern, debugLines);
        // one chat bubble: the split lines rejoin into a single utterance
        std::string joined;
        for (const auto& line : lines)
            joined += (joined.empty() ? "" : " ") + line;
        return joined;
    });
    m_pending.push_back(std::move(p));
}

// ---------------------------------------------------------------- tick

void NpcDialogue::Update()
{
    // Lazy one-time load on the first world tick: config is parsed and the
    // world DB is up by then, and no other init hook exists in the module.
    if (!m_loaded)
        Load();

    if (!m_enabled || m_pending.empty())
        return;

    for (auto it = m_pending.begin(); it != m_pending.end(); )
    {
        if (!it->reply.valid())
        {
            it = m_pending.erase(it);
            continue;
        }

        // non-blocking: never stall the world thread on the model
        if (it->reply.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
        {
            ++it;
            continue;
        }

        std::string text;
        try
        {
            text = it->reply.get();
        }
        catch (...)
        {
            text.clear();
        }

        // The creature may have despawned, died or moved map while we waited.
        // Re-resolve by GUID rather than holding a pointer across threads.
        if (!text.empty())
        {
            if (Map* map = sMapMgr.FindMap(it->mapId))
            {
                if (Creature* c = map->GetCreature(it->creature))
                {
                    if (c->IsAlive())
                        Speak(c, text);
                }
            }
        }

        it = m_pending.erase(it);
    }
}
