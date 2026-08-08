#pragma once
#include "playerbot/ServerFacade.h"

namespace ai
{
    // Per-bot profession steering: when the random-bot event values
    // firstSkill/secondSkill are set, rank-1 primary-profession spells
    // outside the assignment are not trainable (and the sub-10 block is
    // waived for assigned ones). Unassigned bots keep vanilla behavior.
    bool IsTradeSkillAllowedForBot(Player* bot, uint32 learnedSpellId);

    // trainableSpellMap[TrainerType][required class/race/skill][TrainerSpell] = {trainer entry};
    typedef std::unordered_map<TrainerSpell const*, std::vector<int32>>  spellTrainerMap;
    typedef std::unordered_map<uint32, spellTrainerMap>          trainableSpellList;
    typedef std::unordered_map<TrainerType, trainableSpellList>         trainableSpellMap;

    class TrainableSpellMapValue : public SingleCalculatedValue<trainableSpellMap*>
    {
    public:
        TrainableSpellMapValue(PlayerbotAI* ai) : SingleCalculatedValue<trainableSpellMap*>(ai, "trainable spell map") {}

        virtual trainableSpellMap* Calculate() override;
        virtual ~TrainableSpellMapValue() override { delete value; }
    };

    class TrainableSpellsValue : public CalculatedValue<std::vector< TrainerSpell const*>>, public Qualified
    {
    public:
        TrainableSpellsValue(PlayerbotAI* ai, std::string name = "trainable spells", int checkInterval = 5) : CalculatedValue<std::vector<TrainerSpell const*>>(ai, name, checkInterval), Qualified(){}

        virtual std::vector<TrainerSpell const*> Calculate() override;

        virtual std::string Format() override;
    };

    class AvailableTrainersValue : public CalculatedValue<std::vector<int32>>, public Qualified
    {
    public:
        AvailableTrainersValue(PlayerbotAI* ai, std::string name = "available trainers", int checkInterval = 5) : CalculatedValue<std::vector<int32>>(ai, name, checkInterval), Qualified() {}

        virtual std::vector<int32> Calculate() override;
    };

    class TrainCostValue : public Uint32CalculatedValue, public Qualified
    {
    public:
        TrainCostValue(PlayerbotAI* ai) : Uint32CalculatedValue(ai, "train cost", 60), Qualified() {}

        virtual uint32 Calculate() override;
    };
}

