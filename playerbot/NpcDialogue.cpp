#include "NpcDialogue.h"

#include "PlayerbotAIConfig.h"
#include "PlayerbotLLMInterface.h"
#include "strategy/actions/SayAction.h"

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

// A reply opens a 90-second conversation window with that one player; each
// further reply refreshes it. Within the window the player's follow-ups skip
// the cooldown and are treated as questions for the model.
static const uint32 NPC_DIALOGUE_ENGAGE_S = 90;

bool NpcDialogue::IsEngagedWith(ObjectGuid creature, ObjectGuid player, uint32 now) const
{
    auto it = m_engaged.find(creature.GetRawValue());
    return it != m_engaged.end()
        && it->second.first == player.GetRawValue()
        && it->second.second > now;
}

void NpcDialogue::SetEngaged(ObjectGuid creature, ObjectGuid player, uint32 now)
{
    m_engaged[creature.GetRawValue()] = { player.GetRawValue(), now + NPC_DIALOGUE_ENGAGE_S };

    if (m_engaged.size() > 4096)
    {
        for (auto it = m_engaged.begin(); it != m_engaged.end(); )
            it = (it->second.second <= now) ? m_engaged.erase(it) : ++it;
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

void NpcDialogue::DispatchStoryteller(Creature* creature, Player* player,
                                      const std::string& msg)
{
    std::string body = std::string("{\"player\": \"")
        + PlayerbotLLMInterface::SanitizeForJson(player->GetName())
        + "\", \"text\": \"" + PlayerbotLLMInterface::SanitizeForJson(msg) + "\"}";
    Pending p;
    p.creature = creature->GetObjectGuid();
    p.listener = player->GetObjectGuid();
    p.mapId = creature->GetMapId();
    p.playerName = player->GetName();
    p.heard = msg;
    p.storyteller = true;
    std::string url = sPlayerbotAIConfig.storytellerUrl;
    std::string tok = sPlayerbotAIConfig.storytellerToken;
    p.reply = std::async(std::launch::async, [body, url, tok]()
    {
        return PlayerbotLLMInterface::Post(url, body, tok, 10);
    });
    m_pending.push_back(std::move(p));
}

// Just enough JSON reading for the bridge's fixed reply shape:
// {"lines": ["...", ...], "outcome": "...", "devoured": bool}
static std::vector<std::string> ExtractJsonStrings(const std::string& json,
                                                   const std::string& arrayKey)
{
    std::vector<std::string> out;
    size_t at = json.find("\"" + arrayKey + "\"");
    if (at == std::string::npos)
        return out;
    at = json.find('[', at);
    size_t end = json.find(']', at);
    if (at == std::string::npos || end == std::string::npos)
        return out;
    size_t i = at;
    while (i < end)
    {
        size_t q1 = json.find('"', i + 1);
        if (q1 == std::string::npos || q1 > end)
            break;
        std::string piece;
        size_t j = q1 + 1;
        while (j < end)
        {
            char ch = json[j];
            if (ch == '\\' && j + 1 < end)
            {
                char nx = json[j + 1];
                piece += (nx == 'n') ? ' ' : nx;
                j += 2;
                continue;
            }
            if (ch == '"')
                break;
            piece += ch;
            ++j;
        }
        if (!piece.empty())
            out.push_back(piece);
        i = j;
    }
    return out;
}

static std::string ExtractJsonValue(const std::string& json, const std::string& key)
{
    size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos)
        return "";
    at = json.find(':', at);
    if (at == std::string::npos)
        return "";
    size_t q1 = json.find('"', at);
    size_t comma = json.find_first_of(",}", at);
    if (q1 != std::string::npos && (comma == std::string::npos || q1 < comma))
    {
        size_t q2 = json.find('"', q1 + 1);
        return q2 == std::string::npos ? "" : json.substr(q1 + 1, q2 - q1 - 1);
    }
    std::string raw = json.substr(at + 1, (comma == std::string::npos ? json.size() : comma) - at - 1);
    raw.erase(0, raw.find_first_not_of(" 	"));
    raw.erase(raw.find_last_not_of(" 	") + 1);
    return raw;
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

    // Voice anchoring: the exported barks are the character's own words, and
    // a couple of them in the prompt keep the model speaking in that voice
    // instead of generic-assistant. Without this, every NPC sounds like the
    // same helpful concierge no matter who they are.
    if (it != m_lines.end())
    {
        std::ostringstream voice;
        uint32 shown = 0;
        for (const auto& v : { &it->second.greet, &it->second.idle })
            for (const auto& line : *v)
            {
                if (shown >= 3 || voice.str().size() > 400)
                    break;
                voice << "\"" << line << "\" ";
                ++shown;
            }
        if (shown)
            pre << "How you talk (stay in this voice): " << voice.str();
    }

    if (!knowledge.empty())
        pre << "What you know: " << knowledge << " ";

    // Local color: per-zone lore distilled from the era corpus by the
    // worldservice. Absent file means the NPC gets by on its blurb alone.
    {
        std::ifstream lore("/srv/mangos/worldservice/npc_lore/"
                           + std::to_string(creature->GetZoneId()) + ".txt");
        if (lore.is_open())
        {
            std::string line, text;
            while (std::getline(lore, line) && text.size() < 600)
                text += line + " ";
            if (!text.empty())
                pre << "About this place: " << text;
        }
    }

    // The conversation so far, so follow-ups build instead of cold-starting.
    {
        auto hit = m_history.find({ creature->GetObjectGuid().GetRawValue(),
                                    player->GetObjectGuid().GetRawValue() });
        if (hit != m_history.end() && !hit->second.empty())
        {
            pre << "Your conversation so far: ";
            for (const auto& ex : hit->second)
                pre << player->GetName() << " said \"" << ex.first
                    << "\" and you answered \"" << ex.second << "\". ";
        }
    }

    pre << "Answer the question actually asked, in one or two short sentences, "
           "with your own opinions and manner. If you truly do not know, admit "
           "it in your own words - never invent facts, and name at most one "
           "person who might know instead of listing several.";

    // Remembered regulars (issue #58 step 3): the worldservice folds the
    // conversation log into per-NPC, per-player memory files; reading one
    // here means the innkeeper greets a third-visit player like the regular
    // they are. A tiny file read on the world thread, and only when the
    // pair has actually spoken before.
    {
        std::string npcSafe, playerSafe;
        for (char ch : std::string(creature->GetName()))
            npcSafe += (isalnum((unsigned char)ch) ? (char)tolower((unsigned char)ch) : '_');
        for (char ch : std::string(player->GetName()))
            playerSafe += (isalnum((unsigned char)ch) ? (char)tolower((unsigned char)ch) : '_');
        std::ifstream mem("/srv/mangos/worldservice/npc_memory/" + npcSafe + "__" + playerSafe + ".txt");
        if (mem.is_open())
        {
            std::string line, memory;
            while (std::getline(mem, line) && memory.size() < 500)
                memory += line + " ";
            if (!memory.empty())
                pre << "What you remember of this visitor: " << memory;
        }
    }

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

void NpcDialogue::RecordExchange(ObjectGuid creature, ObjectGuid player,
                                 const std::string& heard, const std::string& said)
{
    // Bounded two ways: three exchanges per pair, and a crude full reset if
    // the map ever grows past any plausible number of live conversations.
    if (m_history.size() > 256)
        m_history.clear();
    auto& deque = m_history[{ creature.GetRawValue(), player.GetRawValue() }];
    deque.emplace_back(heard, said);
    while (deque.size() > 3)
        deque.pop_front();
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

    // A player the NPC just replied to is mid-conversation: no cooldown, and
    // everything they say goes to the model. Everyone else waits their turn.
    bool engaged = IsEngagedWith(listener->GetObjectGuid(), player->GetObjectGuid(), now);
    if (!engaged && OnCooldown(listener->GetObjectGuid(), now))
        return;

    // The Storyteller (issue #59): one naga at the end of the world whose
    // conversation IS a text adventure. Everything he hears goes to the tale
    // bridge; the engage window keeps a session playable past the cooldown.
    if (listener->GetEntry() == sPlayerbotAIConfig.storytellerEntry
        && !sPlayerbotAIConfig.storytellerToken.empty())
    {
        for (const auto& pend : m_pending)
            if (pend.creature == listener->GetObjectGuid())
                return;                  // let the current passage finish
        DispatchStoryteller(listener, player, msg);
        SetCooldown(listener->GetObjectGuid(), now);
        SetEngaged(listener->GetObjectGuid(), player->GetObjectGuid(), now);
        return;
    }

    const std::string intent = Classify(msg);

    // Tier 1 -- free, instant, grounded. Greetings and passing remarks never
    // reach the model. A bark also opens the conversation window, so the
    // follow-up ("how are you?") escalates to a real reply.
    if (!engaged && intent != "question")
    {
        std::string line = PickLine(listener->GetEntry(), intent);
        if (!line.empty())
        {
            Speak(listener, line);
            SetCooldown(listener->GetObjectGuid(), now);
            SetEngaged(listener->GetObjectGuid(), player->GetObjectGuid(), now);
            // These conversations used to exist only on the player's screen.
            // Same file and shape as guild-bot chat; channel "npc" so the
            // distiller and future per-NPC memory can filter them cleanly.
            ChatReplyAction::LogLlmConversation(listener->GetName(), player->GetName(),
                "npc", listener->GetZoneId(), msg, line);
            RecordExchange(listener->GetObjectGuid(), player->GetObjectGuid(), msg, line);
        }
        return;
    }

    // Already composing a reply for this creature: let it finish rather than
    // stacking a second generation (the player spoke again while it thought).
    for (const auto& pend : m_pending)
        if (pend.creature == listener->GetObjectGuid())
            return;

    // Tier 2 -- a real question. Bounded: NPCs may hold only part of the
    // generation budget so bot conversation never starves.
    size_t llmPending = 0;
    for (const auto& pend : m_pending)
        if (!pend.storyteller)
            ++llmPending;
    if (llmPending >= sPlayerbotAIConfig.npcDialogueMaxConcurrent)
    {
        std::string line = PickLine(listener->GetEntry(), "deflect");
        if (!line.empty())
        {
            Speak(listener, line);          // bark beats standing mute
            SetCooldown(listener->GetObjectGuid(), now);
            SetEngaged(listener->GetObjectGuid(), player->GetObjectGuid(), now);
            ChatReplyAction::LogLlmConversation(listener->GetName(), player->GetName(),
                "npc", listener->GetZoneId(), msg, line);
            RecordExchange(listener->GetObjectGuid(), player->GetObjectGuid(), msg, line);
        }
        return;
    }

    SetCooldown(listener->GetObjectGuid(), now);
    SetEngaged(listener->GetObjectGuid(), player->GetObjectGuid(), now);

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
    p.playerName = player->GetName();
    p.heard = msg;
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
                    if (c->IsAlive() && it->storyteller)
                    {
                        // The tale bridge replied: speak the passage, and if
                        // the visitor got themselves killed in the story, the
                        // teller takes it personally. Faction 14 with
                        // TEMPFACTION_RESTORE_RESPAWN is self-resetting: he
                        // kills you, evades home, and will tell it again.
                        std::vector<std::string> lines = ExtractJsonStrings(text, "lines");
                        for (const std::string& line : lines)
                            c->MonsterSay(line.c_str(), LANG_UNIVERSAL);
                        SetEngaged(it->creature, it->listener, uint32(time(nullptr)));
                        if (ExtractJsonValue(text, "outcome") == "died")
                        {
                            bool devoured = ExtractJsonValue(text, "devoured") == "true";
                            std::string gloat = devoured
                                ? "You let it EAT you. In MY tale. Unforgivable, landwalker."
                                : "The tale was in my keeping, and you SPOILED the telling!";
                            c->MonsterYell(gloat.c_str(), LANG_UNIVERSAL);
                            c->SetFactionTemporary(14, TEMPFACTION_RESTORE_RESPAWN);
                            if (Player* victim = c->GetMap()->GetPlayer(it->listener))
                                if (c->AI())
                                    c->AI()->AttackStart(victim);
                        }
                        // outcome "idle" arrives with no lines: silence, and
                        // his next hearing falls to ordinary dialogue anyway.
                    }
                    else if (c->IsAlive())
                    {
                        Speak(c, text);
                        // the window runs from when the NPC actually answered,
                        // not from when the question was asked
                        SetEngaged(it->creature, it->listener, uint32(time(nullptr)));
                        ChatReplyAction::LogLlmConversation(c->GetName(), it->playerName,
                            "npc", c->GetZoneId(), it->heard, text);
                        RecordExchange(it->creature, it->listener, it->heard, text);
                    }
                }
            }
        }

        it = m_pending.erase(it);
    }
}
