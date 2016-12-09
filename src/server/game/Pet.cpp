/*
 * Copyright (C) 2005-2008 MaNGOS <http://www.mangosproject.org/>
 *
 * Copyright (C) 2008 Trinity <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "Log.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "Pet.h"
#include "MapManager.h"
#include "Formulas.h"
#include "SpellAuras.h"
#include "CreatureAI.h"
#include "Unit.h"
#include "Util.h"

#define SPELL_WATER_ELEMENTAL_WATERBOLT 31707
#define SPELL_PET_RECENTLY_DISMISSED 47531

char const* petTypeSuffix[MAX_PET_TYPE] =
{
    "'s Minion",                                            // SUMMON_PET
    "'s Pet",                                               // HUNTER_PET
    "'s Guardian",                                          // GUARDIAN_PET
    "'s Companion"                                          // MINI_PET
};

//numbers represent minutes * 100 while happy (you get 100 loyalty points per min while happy)
uint32 const LevelUpLoyalty[6] =
{
    5500,
    11500,
    17000,
    23500,
    31000,
    39500,
};

uint32 const LevelStartLoyalty[6] =
{
    2000,
    4500,
    7000,
    10000,
    13500,
    17500,
};

Pet::Pet(PetType type) : Creature()
{
    m_unitTypeMask |= UNIT_MASK_PET;
    if (type == HUNTER_PET)
        m_unitTypeMask |= UNIT_MASK_HUNTER_PET;

    m_name = "Pet";
    m_petType = type;

    m_removed = false;
    m_regenTimer = 4000;
    m_happinessTimer = 7500;
    m_loyaltyTimer = 12000;
    m_duration = 0;
    m_bonusdamage = 0;

    m_loyaltyPoints = 0;
    m_TrainingPoints = 0;
    m_resetTalentsCost = 0;
    m_resetTalentsTime = 0;

    m_auraUpdateMask = 0;

    // pets always have a charminfo, even if they are not actually charmed
    if (!(m_unitTypeMask & UNIT_MASK_CONTROLABLE_GUARDIAN))
    {
        m_unitTypeMask |= UNIT_MASK_CONTROLABLE_GUARDIAN;
        InitCharmInfo();
    }

    if(type == MINI_PET || type == POSSESSED_PET)                                    // always passive
        SetReactState(REACT_PASSIVE);
    else if(type == GUARDIAN_PET)                           // always aggressive
        SetReactState(REACT_AGGRESSIVE);

    m_spells.clear();
    m_Auras.clear();
    m_CreatureSpellCooldowns.clear();
    m_CreatureCategoryCooldowns.clear();
    m_autospells.clear();
    m_declinedname = nullptr;
    //m_isActive = true;
}

Pet::~Pet()
{
    if(m_uint32Values)                                      // only for fully created Object
    {
        for (auto & m_spell : m_spells)
            delete m_spell.second;
        sObjectAccessor->RemoveObject(this);
    }

    delete m_declinedname;
}

void Pet::AddToWorld()
{
    ///- Register the pet for guid lookup
    if(!IsInWorld())
    {   
        sObjectAccessor->AddObject(this);
        Unit::AddToWorld();
    }
}

void Pet::RemoveFromWorld()
{
    ///- Remove the pet from the accessor
    if(IsInWorld())
    {
        sObjectAccessor->RemoveObject(this);
        ///- Don't call the function for Creature, normal mobs + totems go in a different storage
        Unit::RemoveFromWorld();
    }
}

bool Pet::LoadPetFromDB(Player* owner, uint32 petentry, uint32 petnumber, bool current )
{
    uint32 ownerid = owner->GetGUIDLow();
    Unit* target = nullptr;

    QueryResult result;

    if(petnumber)
        // known petnumber entry                  0   1      2      3        4      5    6           7              8        9           10    11    12       13         14       15            16      17              18        19                 20                 21              22
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, loyaltypoints, loyalty, trainpoint, slot, name, renamed, curhealth, curmana, curhappiness, abdata, TeachSpelldata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType FROM character_pet WHERE owner = '%u' AND id = '%u'",ownerid, petnumber);
    else if(current)
        // current pet (slot 0)                   0   1      2      3        4      5    6           7              8        9           10    11    12       13         14       15            16      17              18        19                 20                 21              22
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, loyaltypoints, loyalty, trainpoint, slot, name, renamed, curhealth, curmana, curhappiness, abdata, TeachSpelldata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType FROM character_pet WHERE owner = '%u' AND slot = '%u'", ownerid, PET_SAVE_AS_CURRENT);
    else if(petentry)
        // known petentry entry (unique for summoned pet, but non unique for hunter pet (only from current or not stabled pets)
        //                                        0   1      2      3        4      5    6           7              8        9           10    11    12       13         14       15            16      17              18        19                 20                 21              22
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, loyaltypoints, loyalty, trainpoint, slot, name, renamed, curhealth, curmana, curhappiness, abdata, TeachSpelldata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType FROM character_pet WHERE owner = '%u' AND entry = '%u' AND (slot = '%u' OR slot > '%u') ", ownerid, petentry, PET_SAVE_AS_CURRENT, PET_SAVE_LAST_STABLE_SLOT );
    else
        // any current or other non-stabled pet (for hunter "call pet")
        //                                        0   1      2      3        4      5    6           7              8        9           10    11    12       13         14       15            16      17              18        19                 20                 21              22
        result = CharacterDatabase.PQuery("SELECT id, entry, owner, modelid, level, exp, Reactstate, loyaltypoints, loyalty, trainpoint, slot, name, renamed, curhealth, curmana, curhappiness, abdata, TeachSpelldata, savetime, resettalents_cost, resettalents_time, CreatedBySpell, PetType FROM character_pet WHERE owner = '%u' AND (slot = '%u' OR slot > '%u') ",ownerid, PET_SAVE_AS_CURRENT, PET_SAVE_LAST_STABLE_SLOT);

    if(!result)
        return false;

    Field *fields = result->Fetch();

    // update for case of current pet "slot = 0"
    petentry = fields[1].GetUInt32();
    if(!petentry)
    {
        return false;
    }

    uint32 summon_spell_id = fields[21].GetUInt32();
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(summon_spell_id);

    bool is_temporary_summoned = spellInfo && spellInfo->GetDuration() > 0;

    // check temporary summoned pets like mage water elemental
    if(current && is_temporary_summoned)
    {
        return false;
    }

    Map *map = owner->GetMap();
    uint32 guid = sObjectMgr->GenerateLowGuid(HIGHGUID_PET);
    uint32 pet_number = fields[0].GetUInt32();
    if(!Create(guid, map, petentry, pet_number))
    {
        return false;
    }

    if (current && owner->IsPetNeedBeTemporaryUnsummoned())
    {
        owner->SetTemporaryUnsummonedPetNumber(pet_number);
        return false;
    }

    //Shadowfiend hack, try to spawn it close to target
    float px, py, pz;
    if (petentry == 19668 && owner->ToPlayer() && owner->ToPlayer()->GetTarget()) 
    {
        target = sObjectAccessor->GetObjectInWorld(owner->ToPlayer()->GetTarget(), (Unit*)nullptr);
        if (target && CanAttack(target) == CAN_ATTACK_RESULT_OK)
        {
            target->GetClosePoint(px, py, pz, GetObjectSize(), PET_FOLLOW_DIST, this->GetFollowAngle());
            UpdateAllowedPositionZ(px, py, pz); //prevent it spawning on flying targets
        }
        else {
            //spawn at owner instead
            owner->GetClosePoint(px, py, pz, GetObjectSize(), PET_FOLLOW_DIST, this->GetFollowAngle());
            target = nullptr;
        }
    }
    else
        owner->GetClosePoint(px, py, pz, GetObjectSize(), PET_FOLLOW_DIST, this->GetFollowAngle());

    Relocate(px, py, pz, owner->GetOrientation());

    if(!IsPositionValid())
    {
        TC_LOG_ERROR("entities.pet","ERROR: Pet (guidlow %d, entry %d) not loaded. Suggested coordinates isn't valid (X: %f Y: %f)",
            GetGUIDLow(), GetEntry(), GetPositionX(), GetPositionY());
        return false;
    }

    setPetType(PetType(fields[22].GetUInt8()));
    SetUInt32Value(UNIT_FIELD_FACTIONTEMPLATE,owner->GetFaction());
    SetUInt32Value(UNIT_CREATED_BY_SPELL, summon_spell_id);

    CreatureTemplate const *cinfo = GetCreatureTemplate();
    if(cinfo->type == CREATURE_TYPE_CRITTER)
    {
        AIM_Initialize();
        map->Add(this->ToCreature(), true);
        return true;
    }
    if(getPetType()==HUNTER_PET || (getPetType()==SUMMON_PET && cinfo->type == CREATURE_TYPE_DEMON && owner->GetClass() == CLASS_WARLOCK))
        m_charmInfo->SetPetNumber(pet_number, true);
    else
        m_charmInfo->SetPetNumber(pet_number, false);

    SetUInt64Value(UNIT_FIELD_SUMMONEDBY, owner->GetGUID());
    SetDisplayId(fields[3].GetUInt32());
    SetNativeDisplayId(fields[3].GetUInt32());
    uint32 petlevel=fields[4].GetUInt8();
    SetUInt32Value(UNIT_NPC_FLAGS , 0);
    SetName(fields[11].GetString());

    switch(getPetType())
    {

        case SUMMON_PET:
            petlevel=owner->GetLevel();

            SetUInt32Value(UNIT_FIELD_BYTES_0,2048);
            SetUInt32Value(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
                                                            // this enables popup window (pet dismiss, cancel)
            break;
        case HUNTER_PET:
            SetUInt32Value(UNIT_FIELD_BYTES_0, 0x02020100);
            SetByteValue(UNIT_FIELD_BYTES_1, 1, fields[8].GetUInt32());
            SetByteValue(UNIT_FIELD_BYTES_2, 0, SHEATH_STATE_MELEE );
            //SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3 | UNIT_BYTE2_FLAG_AURAS | UNIT_BYTE2_FLAG_UNK5 );
            SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3);

            if(fields[12].GetBool())
                SetByteValue(UNIT_FIELD_BYTES_2, 2, UNIT_RENAME_NOT_ALLOWED);
            else
                SetByteValue(UNIT_FIELD_BYTES_2, 2, UNIT_RENAME_ALLOWED);

            SetUInt32Value(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
                                                            // this enables popup window (pet abandon, cancel)
            SetTP(fields[9].GetInt32());
            SetMaxPower(POWER_HAPPINESS,GetCreatePowers(POWER_HAPPINESS));
            SetPower(   POWER_HAPPINESS,fields[15].GetUInt32());
            SetPowerType(POWER_FOCUS);
            break;
        default:
            TC_LOG_ERROR("entities.pet","Pet have incorrect type (%u) for pet loading.",getPetType());
    }
    InitStatsForLevel( petlevel);
    SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, time(nullptr));
    SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, fields[5].GetUInt32());
    SetCreatorGUID(owner->GetGUID());

    SetReactState( ReactStates( fields[6].GetUInt8() ));
    m_loyaltyPoints = fields[7].GetInt32();

    uint32 savedhealth = fields[13].GetUInt32();
    uint32 savedmana = fields[14].GetUInt32();

    // set current pet as current
    if(fields[10].GetUInt8() != 0)
    {
        SQLTransaction trans = CharacterDatabase.BeginTransaction();
        trans->PAppend("UPDATE character_pet SET slot = '%u' WHERE owner = '%u' AND slot = '0' AND id <> '%u'", PET_SAVE_NOT_IN_SLOT, ownerid, m_charmInfo->GetPetNumber());
        trans->PAppend("UPDATE character_pet SET slot = '%u' WHERE owner = '%u' AND id = '%u'", PET_SAVE_AS_CURRENT, ownerid, m_charmInfo->GetPetNumber());
        CharacterDatabase.CommitTransaction(trans);
    }

    if(!is_temporary_summoned)
    {
        // permanent controlled pets store state in DB
        Tokens tokens = StrSplit(fields[16].GetString(), " ");

        if(tokens.size() != 20)
        {
            return false;
        }

        int index;
        Tokens::iterator iter;
        for(iter = tokens.begin(), index = 0; index < 10; ++iter, ++index )
        {
            m_charmInfo->GetActionBarEntry(index)->Type = atol((*iter).c_str());
            ++iter;
            m_charmInfo->GetActionBarEntry(index)->SpellOrAction = atol((*iter).c_str());

            // patch for old data where some spells have ACT_DECIDE but should have ACT_CAST
            // so overwrite old state
            SpellInfo const *spellInfo = sSpellMgr->GetSpellInfo(m_charmInfo->GetActionBarEntry(index)->SpellOrAction);
            if (spellInfo && spellInfo->HasAttribute(SPELL_ATTR1_UNAUTOCASTABLE_BY_PET)) m_charmInfo->GetActionBarEntry(index)->Type = ACT_CAST;
        }

        //init teach spells
        tokens = StrSplit(fields[17].GetString(), " ");
        for (iter = tokens.begin(), index = 0; index < 4; ++iter, ++index)
        {
            uint32 tmp = atol((*iter).c_str());

            ++iter;

            if(tmp)
                AddTeachSpell(tmp, atol((*iter).c_str()));
            else
                break;
        }
    }

    // since last save (in seconds)
    uint32 timediff = (time(nullptr) - fields[18].GetUInt64());

    //load spells/cooldowns/auras
    SetCanModifyStats(true);
    if(getPetType() == HUNTER_PET)
        _LoadAuras(timediff);

    //init AB
    if(is_temporary_summoned)
    {
        // Temporary summoned pets always have initial spell list at load
        InitPetCreateSpells();
    }
    else
    {
        LearnPetPassives();
        if (map->IsBattleArena())
            RemoveArenaAuras();
        CastPetAuras(current);
    }

    AIM_Initialize();

    if(getPetType() == SUMMON_PET && !current)              //all (?) summon pets come with full health when called, but not when they are current
    {
        SetHealth(GetMaxHealth());
        SetPower(POWER_MANA, GetMaxPower(POWER_MANA));
    }
    else
    {
        if(!savedhealth && getPetType() == HUNTER_PET)
            SetDeathState(JUST_DIED);
        else
        {
            SetHealth(savedhealth > GetMaxHealth() ? GetMaxHealth() : (!savedhealth ? 1 : savedhealth) );
            SetPower(POWER_MANA, savedmana > GetMaxPower(POWER_MANA) ? GetMaxPower(POWER_MANA) : savedmana);
        }
    }

    map->Add(this->ToCreature(), true);

    // Spells should be loaded after pet is added to map, because in CanCast is check on it
    _LoadSpells();
    _LoadSpellCooldowns();

    owner->SetPet(this);                                    // in DB stored only full controlled creature

    if(owner->GetTypeId() == TYPEID_PLAYER)
    {
        (owner->ToPlayer())->PetSpellInitialize();
        if((owner->ToPlayer())->GetGroup())
            (owner->ToPlayer())->SetGroupUpdateFlag(GROUP_UPDATE_PET);
    }

    if(owner->GetTypeId() == TYPEID_PLAYER && getPetType() == HUNTER_PET)
    {
        result = CharacterDatabase.PQuery("SELECT genitive, dative, accusative, instrumental, prepositional FROM character_pet_declinedname WHERE owner = '%u' AND id = '%u'", owner->GetGUIDLow(), GetCharmInfo()->GetPetNumber());

        if(result)
        {
            if(m_declinedname)
                delete m_declinedname;

            m_declinedname = new DeclinedName;
            Field *fields = result->Fetch();
            for(int i = 0; i < MAX_DECLINED_NAME_CASES; ++i)
            {
                m_declinedname->name[i] = fields[i].GetString();
            }
        }
    }
    
    if (target)
        AI()->AttackStart(target);

    return true;
}

void Pet::SavePetToDB(PetSaveMode mode)
{
    if(!GetEntry())
        return;

    // save only fully controlled creature
    if(!isControlled())
        return;

    // dont save not player pets
    if (!IS_PLAYER_GUID(GetOwnerGUID()))
        return;

    uint32 curhealth = GetHealth();
    uint32 curmana = GetPower(POWER_MANA);

    //only alive and active pets get auras saved
    if((mode != PET_SAVE_AS_CURRENT && mode != PET_SAVE_NOT_IN_SLOT) || !IsAlive())
        RemoveAllAuras();

    _SaveSpells();
    _SaveSpellCooldowns();
    if(getPetType() == HUNTER_PET)
        _SaveAuras();

    if (mode >= PET_SAVE_AS_CURRENT) //every mode but PET_SAVE_AS_DELETED
    {
        uint32 owner = GUID_LOPART(GetOwnerGUID());
        std::string name = m_name;
        CharacterDatabase.EscapeString(name);
        SQLTransaction trans = CharacterDatabase.BeginTransaction();
        // remove current data
        trans->PAppend("DELETE FROM character_pet WHERE owner = '%u' AND id = '%u'", owner,m_charmInfo->GetPetNumber() );

        // prevent duplicate using slot (except PET_SAVE_NOT_IN_SLOT)
        if(mode!=PET_SAVE_NOT_IN_SLOT)
            trans->PAppend("UPDATE character_pet SET slot = 3 WHERE owner = '%u' AND slot = '%u'", owner, uint32(mode) );

        // prevent existence another hunter pet in PET_SAVE_AS_CURRENT and PET_SAVE_NOT_IN_SLOT
        if(getPetType()==HUNTER_PET && (mode==PET_SAVE_AS_CURRENT||mode==PET_SAVE_NOT_IN_SLOT))
            trans->PAppend("DELETE FROM character_pet WHERE owner = '%u' AND (slot = '%u' OR slot > '%u')", owner, PET_SAVE_AS_CURRENT, PET_SAVE_LAST_STABLE_SLOT );
        // save pet
        std::ostringstream ss;
        ss  << "INSERT INTO character_pet ( id, entry,  owner, modelid, level, exp, Reactstate, loyaltypoints, loyalty, trainpoint, slot, name, renamed, curhealth, curmana, curhappiness, abdata,TeachSpelldata,savetime,resettalents_cost,resettalents_time,CreatedBySpell,PetType) "
            << "VALUES ("
            << m_charmInfo->GetPetNumber() << ", "
            << GetEntry() << ", "
            << owner << ", "
            << GetNativeDisplayId() << ", "
            << GetLevel() << ", "
            << GetUInt32Value(UNIT_FIELD_PETEXPERIENCE) << ", "
            << uint32(GetReactState()) << ", "
            << m_loyaltyPoints << ", "
            << GetLoyaltyLevel() << ", "
            << m_TrainingPoints << ", "
            << uint32(mode) << ", '"
            << name.c_str() << "', "
            << uint32((GetByteValue(UNIT_FIELD_BYTES_2, 2) == UNIT_RENAME_ALLOWED)?0:1) << ", "
            << curhealth << ", "
            << curmana << ", "
            << GetPower(POWER_HAPPINESS) << ", '";

        for(uint32 i = 0; i < 10; i++)
            ss << uint32(m_charmInfo->GetActionBarEntry(i)->Type) << " " << uint32(m_charmInfo->GetActionBarEntry(i)->SpellOrAction) << " ";
        ss << "', '";

        //save spells the pet can teach to it's Master
        {
            int i = 0;
            for(auto itr = m_teachspells.begin(); i < 4 && itr != m_teachspells.end(); ++i, ++itr)
                ss << itr->first << " " << itr->second << " ";
            for(; i < 4; ++i)
                ss << uint32(0) << " " << uint32(0) << " ";
        }

        ss  << "', "
            << time(nullptr) << ", "
            << uint32(m_resetTalentsCost) << ", "
            << uint64(m_resetTalentsTime) << ", "
            << GetUInt32Value(UNIT_CREATED_BY_SPELL) << ", "
            << uint32(getPetType()) << ")";

        trans->Append( ss.str().c_str() );

        CharacterDatabase.CommitTransaction(trans);
    } else { // PET_SAVE_AS_DELETED
        DeleteFromDB(m_charmInfo->GetPetNumber());
    }
}

void Pet::DeleteFromDB(uint32 guidlow)
{
    SQLTransaction trans = CharacterDatabase.BeginTransaction();
    trans->PAppend("DELETE FROM character_pet WHERE id = '%u'", guidlow);
    trans->PAppend("DELETE FROM character_pet_declinedname WHERE id = '%u'", guidlow);
    trans->PAppend("DELETE FROM pet_aura WHERE guid = '%u'", guidlow);
    trans->PAppend("DELETE FROM pet_spell WHERE guid = '%u'", guidlow);
    trans->PAppend("DELETE FROM pet_spell_cooldown WHERE guid = '%u'", guidlow);
    CharacterDatabase.CommitTransaction(trans);
}

void Pet::SetDeathState(DeathState s)                       // overwrite virtual Creature::setDeathState and Unit::setDeathState
{
    Creature::SetDeathState(s);
    if(GetDeathState()==CORPSE)
    {
        //remove summoned pet (no corpse)
        if(getPetType()==SUMMON_PET)
            Remove(PET_SAVE_NOT_IN_SLOT);
        // other will despawn at corpse desppawning (Pet::Update code)
        else
        {
            // pet corpse non lootable and non skinnable
            SetUInt32Value( UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_NONE );
            RemoveFlag (UNIT_FIELD_FLAGS, UNIT_FLAG_SKINNABLE);

             //lose happiness when died and not in BG/Arena
            if (!GetMap()->IsBattlegroundOrArena())
                ModifyPower(POWER_HAPPINESS, -HAPPINESS_LEVEL_SIZE);

            SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
        }
    }
    else if(GetDeathState()==ALIVE)
    {
        RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_STUNNED);
        CastPetAuras(true);
    }
}

void Pet::Update(uint32 diff)
{
    if(m_removed)                                           // pet already removed, just wait in remove queue, no updates
        return;

    switch( m_deathState )
    {
        case CORPSE:
        {
            if (m_corpseRemoveTime <= time(nullptr))
            {
                assert(getPetType()!=SUMMON_PET && "Must be already removed.");
                if (m_petType != HUNTER_PET)
                    Remove(PET_SAVE_NOT_IN_SLOT);               //hunters' pets never get removed because of death, NEVER!
                return;
            }
            break;
        }
        case ALIVE:
        {
            // unsummon pet that lost owner
            Unit* owner = GetOwner();
            if(!owner || (!IsWithinDistInMap(owner, OWNER_MAX_DISTANCE) && !IsPossessed()) || (isControlled() && !owner->GetMinionGUID()))
            {
                Remove(PET_SAVE_NOT_IN_SLOT, true);
                return;
            }

            if(isControlled())
            {
                if( owner->GetMinionGUID() != GetGUID() )
                {
                    Remove(getPetType()==HUNTER_PET?PET_SAVE_AS_DELETED:PET_SAVE_NOT_IN_SLOT);
                    return;
                }
            }

            if(m_duration > 0)
            {
                if(m_duration > diff)
                    m_duration -= diff;
                else
                {
                    Remove(getPetType() != SUMMON_PET ? PET_SAVE_AS_DELETED:PET_SAVE_NOT_IN_SLOT);
                    return;
                }
            }

            if(getPetType() != HUNTER_PET)
                break;

            //regenerate Focus
            if(m_regenTimer <= diff)
            {
                RegenerateFocus();
                m_regenTimer = 4000;
            }
            else
                m_regenTimer -= diff;

            //don't loose happiness for arena server
            if(!sWorld->getConfig(CONFIG_ARENASERVER_ENABLED))
            {
                if(m_happinessTimer <= diff)
                {
                    LooseHappiness();
                    m_happinessTimer = 7500;
                }
                else
                    m_happinessTimer -= diff;
            }

            if(m_loyaltyTimer <= diff)
            {
                TickLoyaltyChange();
                m_loyaltyTimer = 12000;
            }
            else
                m_loyaltyTimer -= diff;

            break;
        }
        default:
            break;
    }
    Creature::Update(diff);
}

void Pet::RegenerateFocus()
{
    uint32 curValue = GetPower(POWER_FOCUS);
    uint32 maxValue = GetMaxPower(POWER_FOCUS);

    if (curValue >= maxValue)
        return;

    float addvalue = 24 * sWorld->GetRate(RATE_POWER_FOCUS);

    AuraList const& ModPowerRegenPCTAuras = GetAurasByType(SPELL_AURA_MOD_POWER_REGEN_PERCENT);
    for(auto ModPowerRegenPCTAura : ModPowerRegenPCTAuras)
        if (ModPowerRegenPCTAura->GetModifier()->m_miscvalue == POWER_FOCUS)
            addvalue *= (ModPowerRegenPCTAura->GetModifierValue() + 100) / 100.0f;

    ModifyPower(POWER_FOCUS, (int32)addvalue);
}

void Pet::LooseHappiness()
{
    uint32 curValue = GetPower(POWER_HAPPINESS);
    if (curValue <= 0)
        return;
    int32 addvalue = (140 >> GetLoyaltyLevel()) * 125;      //value is 70/35/17/8/4 (per min) * 1000 / 8 (timer 7.5 secs)
    if(IsInCombat())                                        //we know in combat happiness fades faster, multiplier guess
        addvalue = int32(addvalue * 1.5);
    ModifyPower(POWER_HAPPINESS, -addvalue);
}

void Pet::ModifyLoyalty(int32 addvalue)
{
    uint32 loyaltylevel = GetLoyaltyLevel();

    if(addvalue > 0)                                        //only gain influenced, not loss
        addvalue = int32((float)addvalue * sWorld->GetRate(RATE_LOYALTY));

    if(loyaltylevel >= BEST_FRIEND && (addvalue + m_loyaltyPoints) > int32(GetMaxLoyaltyPoints(loyaltylevel)))
        return;

    m_loyaltyPoints += addvalue;

    if(m_loyaltyPoints < 0)
    {
        if(loyaltylevel > REBELLIOUS)
        {
            //level down
            --loyaltylevel;
            SetLoyaltyLevel(LoyaltyLevel(loyaltylevel));
            m_loyaltyPoints = GetStartLoyaltyPoints(loyaltylevel);
            SetTP(m_TrainingPoints - int32(GetLevel()));
        }
        else
        {
            m_loyaltyPoints = 0;
            Unit* owner = GetOwner();
            if(owner && owner->GetTypeId() == TYPEID_PLAYER)
            {
                WorldPacket data(SMSG_PET_BROKEN, 0);
                (owner->ToPlayer())->SendDirectMessage(&data);

                //run away
                (owner->ToPlayer())->RemovePet(this,PET_SAVE_AS_DELETED);
            }
        }
    }
    //level up
    else if(m_loyaltyPoints > int32(GetMaxLoyaltyPoints(loyaltylevel)))
    {
        ++loyaltylevel;
        SetLoyaltyLevel(LoyaltyLevel(loyaltylevel));
        m_loyaltyPoints = GetStartLoyaltyPoints(loyaltylevel);
        SetTP(m_TrainingPoints + GetLevel());
    }
}

void Pet::TickLoyaltyChange()
{
    int32 addvalue;

    switch(GetHappinessState())
    {
        case HAPPY:   addvalue =  20; break;
        case CONTENT: addvalue =  10; break;
        case UNHAPPY: addvalue = -20; break;
        default:
            return;
    }
    ModifyLoyalty(addvalue);
}

void Pet::KillLoyaltyBonus(uint32 level)
{
    if(level > 100)
        return;

                                                            //at lower levels gain is faster | the lower loyalty the more loyalty is gained
    uint32 bonus = uint32(((100 - level) / 10) + (6 - GetLoyaltyLevel()));
    ModifyLoyalty(bonus);
}

HappinessState Pet::GetHappinessState()
{
    if(GetPower(POWER_HAPPINESS) < HAPPINESS_LEVEL_SIZE)
        return UNHAPPY;
    else if(GetPower(POWER_HAPPINESS) >= HAPPINESS_LEVEL_SIZE * 2)
        return HAPPY;
    else
        return CONTENT;
}

void Pet::SetLoyaltyLevel(LoyaltyLevel level)
{
    SetByteValue(UNIT_FIELD_BYTES_1, 1, level);
}

bool Pet::CanTakeMoreActiveSpells(uint32 spellid)
{
    uint8  activecount = 1;
    uint32 chainstartstore[ACTIVE_SPELLS_MAX];

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
    if(spellInfo && spellInfo->IsPassive())
        return true;

    chainstartstore[0] = sSpellMgr->GetFirstSpellInChain(spellid);

    for (auto & m_spell : m_spells)
    {
        spellInfo = sSpellMgr->GetSpellInfo(m_spell.first);
        if(spellInfo && spellInfo->IsPassive())
            continue;

        uint32 chainstart = sSpellMgr->GetFirstSpellInChain(m_spell.first);

        uint8 x;

        for(x = 0; x < activecount; x++)
        {
            if(chainstart == chainstartstore[x])
                break;
        }

        if(x == activecount)                                //spellchain not yet saved -> add active count
        {
            ++activecount;
            if(activecount > ACTIVE_SPELLS_MAX)
                return false;
            chainstartstore[x] = chainstart;
        }
    }
    return true;
}

bool Pet::HasTPForSpell(uint32 spellid)
{
    int32 neededtrainp = GetTPForSpell(spellid);
    if((m_TrainingPoints - neededtrainp < 0 || neededtrainp < 0) && neededtrainp != 0)
        return false;
    return true;
}

int32 Pet::GetTPForSpell(uint32 spellid)
{
    uint32 basetrainp = 0;

    SkillLineAbilityMapBounds skill_bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellid);
    for(auto _spell_idx = skill_bounds.first; _spell_idx != skill_bounds.second; ++_spell_idx)
    {
        if(!_spell_idx->second->reqtrainpoints)
            return 0;

        basetrainp = _spell_idx->second->reqtrainpoints;
        break;
    }

    uint32 spenttrainp = 0;
    uint32 chainstart = sSpellMgr->GetFirstSpellInChain(spellid);

    for (auto & m_spell : m_spells)
    {
        if(m_spell.second->state == PETSPELL_REMOVED)
            continue;

        if(sSpellMgr->GetFirstSpellInChain(m_spell.first) == chainstart)
        {
            SkillLineAbilityMapBounds skill_bounds = sSpellMgr->GetSkillLineAbilityMapBounds(m_spell.first);
            for(auto _spell_idx2 = skill_bounds.first; _spell_idx2 != skill_bounds.second; ++_spell_idx2)
            {
                if(_spell_idx2->second->reqtrainpoints > spenttrainp)
                {
                    spenttrainp = _spell_idx2->second->reqtrainpoints;
                    break;
                }
            }
        }
    }

    return int32(basetrainp) - int32(spenttrainp);
}

uint32 Pet::GetMaxLoyaltyPoints(uint32 level)
{
    if (!level) {
        TC_LOG_ERROR("entities.pet","CRASH ALERT: Called Pet::GetMaxLoyaltyPoints with level 0 for creature entry %u, owner %s (GUID %u). Incrementing it to prevent crash.", GetEntry(), GetOwner() ? GetOwner()->GetName().c_str() : "unknown", GetOwner() ? GetOwner()->GetGUIDLow() : 0);
        ++level;
    }

    return LevelUpLoyalty[level - 1];
}

uint32 Pet::GetStartLoyaltyPoints(uint32 level)
{
    return LevelStartLoyalty[level - 1];
}

void Pet::SetTP(int32 TP)
{
    m_TrainingPoints = TP;
    SetUInt32Value(UNIT_TRAINING_POINTS, (uint32)GetDispTP());
}

int32 Pet::GetDispTP()
{
    if(getPetType()!= HUNTER_PET)
        return(0);
    if(m_TrainingPoints < 0)
        return -m_TrainingPoints;
    else
        return -(m_TrainingPoints + 1);
}

void Pet::Remove(PetSaveMode mode, bool returnreagent)
{
    Unit* owner = GetOwner();

    if(owner)
    {
        if(owner->GetTypeId()==TYPEID_PLAYER)
        {
            (owner->ToPlayer())->RemovePet(this,mode,returnreagent);
            return;
        }

        // only if current pet in slot
        if(owner->GetMinionGUID()==GetGUID())
            owner->SetPet(nullptr);
    }

    AddObjectToRemoveList();
    m_removed = true;
}

void Pet::GivePetXP(uint32 xp)
{
    if(getPetType() != HUNTER_PET)
        return;

    if ( xp < 1 )
        return;

    if(!IsAlive())
        return;

    uint32 level = GetLevel();

    // XP to money conversion processed in Player::RewardQuest
    if(level >= sWorld->getConfig(CONFIG_MAX_PLAYER_LEVEL))
        return;

    uint32 curXP = GetUInt32Value(UNIT_FIELD_PETEXPERIENCE);
    uint32 nextLvlXP = GetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP);
    uint32 newXP = curXP + xp;

    if(newXP >= nextLvlXP && level+1 > GetOwner()->GetLevel())
    {
        SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, nextLvlXP-1);
        return;
    }

    while( newXP >= nextLvlXP && level < sWorld->getConfig(CONFIG_MAX_PLAYER_LEVEL) )
    {
        newXP -= nextLvlXP;

        SetLevel( level + 1 );
        SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, uint32((Trinity::XP::xp_to_level(level+1))/4));

        level = GetLevel();
        nextLvlXP = GetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP);
        GivePetLevel(level);
    }

    SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, newXP);

    if(getPetType() == HUNTER_PET)
        KillLoyaltyBonus(level);
}

void Pet::GivePetLevel(uint32 level)
{
    if(!level)
        return;

    InitStatsForLevel( level);

    SetTP(m_TrainingPoints + (GetLoyaltyLevel() - 1));
}

bool Pet::CreateBaseAtCreature(Creature* creature)
{
    if(!creature)
    {
        TC_LOG_ERROR("entities.pet","CRITICAL ERROR: NULL pointer parsed into CreateBaseAtCreature()");
        return false;
    }
    uint32 guid=sObjectMgr->GenerateLowGuid(HIGHGUID_PET);

    SetInstanceId(creature->GetInstanceId());

    uint32 pet_number = sObjectMgr->GeneratePetNumber();
    if(!Create(guid, creature->GetMap(), creature->GetEntry(), pet_number))
        return false;

    Relocate(creature->GetPositionX(), creature->GetPositionY(), creature->GetPositionZ(), creature->GetOrientation());

    if(!IsPositionValid())
    {
        TC_LOG_ERROR("entities.pet","ERROR: Pet (guidlow %d, entry %d) not created base at creature. Suggested coordinates isn't valid (X: %f Y: %f)",
            GetGUIDLow(), GetEntry(), GetPositionX(), GetPositionY());
        return false;
    }

    CreatureTemplate const *cinfo = GetCreatureTemplate();
    if(!cinfo)
    {
        TC_LOG_ERROR("entities.pet","ERROR: CreateBaseAtCreature() failed, creatureInfo is missing!");
        return false;
    }

    if(cinfo->type == CREATURE_TYPE_CRITTER)
    {
        setPetType(MINI_PET);
        return true;
    }
    SetDisplayId(creature->GetDisplayId());
    SetNativeDisplayId(creature->GetNativeDisplayId());
    SetMaxPower(POWER_HAPPINESS, GetCreatePowers(POWER_HAPPINESS));
    SetPower(POWER_HAPPINESS, 166500);
    SetPowerType(POWER_FOCUS);
    SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, 0);
    SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
    SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, uint32((Trinity::XP::xp_to_level(creature->GetLevel()))/4));
    SetUInt32Value(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
    SetUInt32Value(UNIT_NPC_FLAGS, 0);

    if(CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cinfo->family))
        SetName(cFamily->Name[sWorld->GetDefaultDbcLocale()]);

    if(GetName().empty())
        SetName(cinfo->Name);

    m_loyaltyPoints = 1000;
    if(cinfo->type == CREATURE_TYPE_BEAST)
    {
        SetUInt32Value(UNIT_FIELD_BYTES_0, 0x02020100);
        SetByteValue(UNIT_FIELD_BYTES_2, 0, SHEATH_STATE_MELEE );
        //SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3 | UNIT_BYTE2_FLAG_AURAS | UNIT_BYTE2_FLAG_UNK5 );
        SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3);
        SetByteValue(UNIT_FIELD_BYTES_2, 2, UNIT_RENAME_ALLOWED);

        SetUInt32Value(UNIT_MOD_CAST_SPEED, creature->GetUInt32Value(UNIT_MOD_CAST_SPEED) );
        SetLoyaltyLevel(REBELLIOUS);
    }
    return true;
}

bool Pet::CreateBaseAtCreatureEntry(uint32 entry, Unit* spawnOn)
{
    if(!entry)
    {
        TC_LOG_ERROR("entities.pet","Pet::CreateBaseAtCreatureEntry : null entry given");
        return false;
    }
    if(!spawnOn)
    {
        TC_LOG_ERROR("entities.pet","Pet::CreateBaseAtCreatureEntry : null unit given");
        return false;
    }

    uint32 guid=sObjectMgr->GenerateLowGuid(HIGHGUID_PET);

    SetInstanceId(spawnOn->GetInstanceId());

    uint32 pet_number = sObjectMgr->GeneratePetNumber();
    if(!Create(guid, spawnOn->GetMap(), entry, pet_number))
        return false;
    
    Relocate(spawnOn->GetPositionX(), spawnOn->GetPositionY(), spawnOn->GetPositionZ(), spawnOn->GetOrientation());

    if(!IsPositionValid())
    {
        TC_LOG_ERROR("entities.pet","ERROR: Pet (guidlow %d, entry %d) not created base at creature. Suggested coordinates isn't valid (X: %f Y: %f)",
            GetGUIDLow(), GetEntry(), GetPositionX(), GetPositionY());
        return false;
    }

    CreatureTemplate const *cinfo = GetCreatureTemplate();
    if(!cinfo)
    {
        TC_LOG_ERROR("entities.pet","ERROR: CreateBaseAtCreature() failed, creatureInfo is missing!");
        return false;
    }

    if(cinfo->type == CREATURE_TYPE_CRITTER)
    {
        setPetType(MINI_PET);
        return true;
    }

    SetDisplayId(cinfo->Modelid1);
    SetNativeDisplayId(cinfo->Modelid1);
    SetMaxPower(POWER_HAPPINESS, GetCreatePowers(POWER_HAPPINESS));
    SetPower(POWER_HAPPINESS, 166500);
    SetPowerType(POWER_FOCUS);
    SetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP, 0);
    SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
    SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, uint32((Trinity::XP::xp_to_level(cinfo->minlevel))/4));
    SetUInt32Value(UNIT_FIELD_FLAGS, UNIT_FLAG_PVP_ATTACKABLE);
    SetUInt32Value(UNIT_NPC_FLAGS, 0);

    if(CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cinfo->family))
        SetName(cFamily->Name[sWorld->GetDefaultDbcLocale()]);

    if(GetName().empty())
        SetName(cinfo->Name);

    m_loyaltyPoints = 1000;
    if(cinfo->type == CREATURE_TYPE_BEAST)
    {
        SetUInt32Value(UNIT_FIELD_BYTES_0, 0x02020100);
        SetByteValue(UNIT_FIELD_BYTES_2, 0, SHEATH_STATE_MELEE );
        //SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3 | UNIT_BYTE2_FLAG_AURAS | UNIT_BYTE2_FLAG_UNK5 );
        SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3);
        SetByteValue(UNIT_FIELD_BYTES_2, 2, UNIT_RENAME_ALLOWED);

        //SetUInt32Value(UNIT_MOD_CAST_SPEED, creature->GetUInt32Value(UNIT_MOD_CAST_SPEED) );
        SetLoyaltyLevel(REBELLIOUS);
    }
    return true;
}

bool Pet::InitStatsForLevel(uint32 petlevel)
{
    CreatureTemplate const *cinfo = GetCreatureTemplate();
    assert(cinfo);

    Unit* owner = GetOwner();
    if(!owner)
    {
        TC_LOG_ERROR("entities.pet","ERROR: attempt to summon pet (Entry %u) without owner! Attempt terminated.", cinfo->Entry);
        return false;
    }

    uint32 creature_ID = (getPetType() == HUNTER_PET) ? 1 : cinfo->Entry;

    SetLevel(petlevel);

    SetMeleeDamageSchool(SpellSchools(cinfo->dmgschool));

    SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, float(petlevel*50));

    SetAttackTime(BASE_ATTACK, BASE_ATTACK_TIME);
    SetAttackTime(OFF_ATTACK, BASE_ATTACK_TIME);
    SetAttackTime(RANGED_ATTACK, BASE_ATTACK_TIME);

    SetFloatValue(UNIT_MOD_CAST_SPEED, 1.0);

    CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cinfo->family);
    if(cFamily && cFamily->minScale > 0.0f && getPetType()==HUNTER_PET)
    {
        float scale;
        if (GetLevel() >= cFamily->maxScaleLevel)
            scale = cFamily->maxScale;
        else if (GetLevel() <= cFamily->minScaleLevel)
            scale = cFamily->minScale;
        else
          scale = cFamily->minScale + (float)(GetLevel() - cFamily->minScaleLevel) / (float)cFamily->maxScaleLevel * (cFamily->maxScale - cFamily->minScale);

        SetFloatValue(OBJECT_FIELD_SCALE_X, scale);
    }
    m_bonusdamage = 0;

    int32 creatureResistance[MAX_SPELL_SCHOOL] = {0,0,0,0,0,0,0};

    if(cinfo && getPetType() != HUNTER_PET)
    {
        creatureResistance[SPELL_SCHOOL_HOLY]   = cinfo->resistance[SPELL_SCHOOL_HOLY - 1];
        creatureResistance[SPELL_SCHOOL_FIRE]   = cinfo->resistance[SPELL_SCHOOL_FIRE - 1];
        creatureResistance[SPELL_SCHOOL_NATURE] = cinfo->resistance[SPELL_SCHOOL_NATURE -1];
        creatureResistance[SPELL_SCHOOL_FROST]  = cinfo->resistance[SPELL_SCHOOL_FROST -1];
        creatureResistance[SPELL_SCHOOL_SHADOW] = cinfo->resistance[SPELL_SCHOOL_SHADOW -1];
        creatureResistance[SPELL_SCHOOL_ARCANE] = cinfo->resistance[SPELL_SCHOOL_ARCANE -1];
    }

    switch(getPetType())
    {
        case SUMMON_PET:
        {
            if(owner->GetTypeId() == TYPEID_PLAYER)
            {
                switch(owner->GetClass())
                {
                    case CLASS_WARLOCK:
                    {

                        //the damage bonus used for pets is either fire or shadow damage, whatever is higher
                        uint32 fire  = owner->GetInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + SPELL_SCHOOL_FIRE);
                        uint32 shadow = owner->GetInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + SPELL_SCHOOL_SHADOW);
                        uint32 val  = (fire > shadow) ? fire : shadow;

                        SetBonusDamage(int32 (val * 0.15f));
                        //bonusAP += val * 0.57;
                        break;
                    }
                    case CLASS_MAGE:
                    {
                        //40% damage bonus of mage's frost damage
                        float val = owner->GetInt32Value(PLAYER_FIELD_MOD_DAMAGE_DONE_POS + SPELL_SCHOOL_FROST) * 0.4;
                        if(val < 0)
                            val = 0;
                        SetBonusDamage( int32(val));
                        break;
                    }
                    default:
                        break;
                }
            }

            SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel - (petlevel / 4)) );
            SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel + (petlevel / 4)) );

            //SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_VALUE, float(cinfo->attackpower));

            PetLevelInfo const* pInfo = sObjectMgr->GetPetLevelInfo(creature_ID, petlevel);
            if(pInfo)                                       // exist in DB
            {
                SetCreateHealth(pInfo->health);
                SetCreateMana(pInfo->mana);

                if(pInfo->armor > 0)
                    SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, float(pInfo->armor));

                for(int stat = 0; stat < MAX_STATS; ++stat)
                {
                    SetCreateStat(Stats(stat), float(pInfo->stats[stat]));
                }
            }
            else                                            // not exist in DB, use some default fake data
            {
                if (owner->GetTypeId() == TYPEID_PLAYER)
                    TC_LOG_ERROR("entities.pet","Summoned pet (Entry: %u) not have pet stats data in DB",cinfo->Entry);

                // remove elite bonuses included in DB values
                CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(petlevel, cinfo->unit_class);
                SetCreateHealth(stats->BaseHealth[cinfo->expansion]);
                SetCreateMana(stats->BaseMana);

                SetCreateStat(STAT_STRENGTH, 22);
                SetCreateStat(STAT_AGILITY, 22);
                SetCreateStat(STAT_STAMINA, 25);
                SetCreateStat(STAT_INTELLECT, 28);
                SetCreateStat(STAT_SPIRIT, 27);
            }
            break;
        }
        case HUNTER_PET:
        {
            SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, uint32((Trinity::XP::xp_to_level(petlevel))/4));

            //these formula may not be correct; however, it is designed to be close to what it should be
            //this makes dps 0.5 of pets level
            SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel - (petlevel / 4)) );
            //damage range is then petlevel / 2
            SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel + (petlevel / 4)) );
            //damage is increased afterwards as strength and pet scaling modify attack power

            //stored standard pet stats are entry 1 in pet_levelinfo
            PetLevelInfo const* pInfo = sObjectMgr->GetPetLevelInfo(creature_ID, petlevel);
            if(pInfo)                                       // exist in DB
            {
                SetCreateHealth(pInfo->health);
                SetModifierValue(UNIT_MOD_ARMOR, BASE_VALUE, float(pInfo->armor));
                //SetModifierValue(UNIT_MOD_ATTACK_POWER, BASE_VALUE, float(cinfo->attackpower));

                for( int i = STAT_STRENGTH; i < MAX_STATS; i++)
                {
                    SetCreateStat(Stats(i),  float(pInfo->stats[i]));
                }
            }
            else                                            // not exist in DB, use some default fake data
            {
                TC_LOG_ERROR("entities.pet","Hunter pet levelstats missing in DB");

                // remove elite bonuses included in DB values
                CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(petlevel, cinfo->unit_class);
                SetCreateHealth(stats->BaseHealth[cinfo->expansion]);
                SetCreateMana(stats->BaseMana);

                SetCreateStat(STAT_STRENGTH, 22);
                SetCreateStat(STAT_AGILITY, 22);
                SetCreateStat(STAT_STAMINA, 25);
                SetCreateStat(STAT_INTELLECT, 28);
                SetCreateStat(STAT_SPIRIT, 27);
            }
            break;
        }
        case GUARDIAN_PET:
            SetUInt32Value(UNIT_FIELD_PETEXPERIENCE, 0);
            SetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP, 1000);

            switch(GetEntry())
            {
                case 1964: //force of nature
                    SetCreateHealth(30 + 30*petlevel);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel * 2.5f - (petlevel / 2)));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel * 2.5f + (petlevel / 2)));
                    break;
                case 15352: //earth elemental 36213
                    SetCreateHealth(100 + 120*petlevel);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel - (petlevel / 4)));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel + (petlevel / 4)));
                    break;
                case 15438: //fire elemental
                    SetCreateHealth(40*petlevel);
                    SetCreateMana(28 + 10*petlevel);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel * 4 - petlevel));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel * 4 + petlevel));
                    break;
                case 19668: // Shadowfiend
                {
                    PetLevelInfo const* pInfo = sObjectMgr->GetPetLevelInfo(creature_ID, petlevel);
                    if (!pInfo) {
                        SetCreateMana(28 + 10*petlevel);
                        SetCreateHealth(28 + 30*petlevel);
                    }
                    int32 bonus_dmg = (int32(owner->SpellBaseDamageBonusDone(SPELL_SCHOOL_MASK_SHADOW)* 0.3f));
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float((petlevel * 4 - petlevel) + bonus_dmg));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float((petlevel * 4 + petlevel) + bonus_dmg));
                    break;
                }
                case 19833: //Snake Trap - Venomous Snake
                    SetCreateHealth(uint32(107 * (petlevel - 40) * 0.025f));
                    SetCreateMana(0);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float((petlevel / 2) - 25));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float((petlevel / 2) - 18));
                    break;
                case 19921: //Snake Trap - Viper
                    SetCreateHealth(uint32(107 * (petlevel - 40) * 0.025f));
                    SetCreateMana(0);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel / 2 - 10));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel / 2));
                    break;
                default:
                    SetCreateMana(28 + 10*petlevel);
                    SetCreateHealth(28 + 30*petlevel);

                    // FIXME: this is wrong formula, possible each guardian pet have own damage formula
                    //these formula may not be correct; however, it is designed to be close to what it should be
                    //this makes dps 0.5 of pets level
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel - (petlevel / 4)));
                    //damage range is then petlevel / 2
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel + (petlevel / 4)));
                    break;
            }
            break;
        case POSSESSED_PET:
            switch(GetEntry())
            {
                case 19405: // Steam Tonk
                    SetCreateHealth(30 + 10*petlevel);
                    SetCreateMana(30 + 10*petlevel);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, 1.0f);
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, 2.0f);
                    break;
                default:
                    SetCreateMana(28 + 10*petlevel);
                    SetCreateHealth(28 + 30*petlevel);
                    SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE, float(petlevel - (petlevel / 4)));
                    SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE, float(petlevel + (petlevel / 4)));
                    break;
            }
            break;
        default:
            TC_LOG_ERROR("entities.pet","Pet have incorrect type (%u) for levelup.", getPetType());
            break;
    }

    for (int i = SPELL_SCHOOL_HOLY; i < MAX_SPELL_SCHOOL; ++i)
        SetModifierValue(UnitMods(UNIT_MOD_RESISTANCE_START + i), BASE_VALUE, float(creatureResistance[i]));

    UpdateAllStats();

    SetHealth(GetMaxHealth());
    SetPower(POWER_MANA, GetMaxPower(POWER_MANA));

    return true;
}

bool Pet::HaveInDiet(ItemTemplate const* item) const
{
    if (!item->FoodType)
        return false;

    CreatureTemplate const* cInfo = GetCreatureTemplate();
    if(!cInfo)
        return false;

    CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cInfo->family);
    if(!cFamily)
        return false;

    uint32 diet = cFamily->petFoodMask;
    uint32 FoodMask = 1 << (item->FoodType-1);
    return diet & FoodMask;
}

uint32 Pet::GetCurrentFoodBenefitLevel(uint32 itemlevel)
{
    // -5 or greater food level
    if(GetLevel() <= itemlevel + 5)                         //possible to feed level 60 pet with level 55 level food for full effect
        return 35000;
    // -10..-6
    else if(GetLevel() <= itemlevel + 10)                   //pure guess, but sounds good
        return 17000;
    // -14..-11
    else if(GetLevel() <= itemlevel + 14)                   //level 55 food gets green on 70, makes sense to me
        return 8000;
    // -15 or less
    else
        return 0;                                           //food too low level
}

void Pet::_LoadSpellCooldowns()
{
    if (GetEntry() == 510) // Don't load cooldowns for mage water elem
        return;
    
    m_CreatureSpellCooldowns.clear();
    m_CreatureCategoryCooldowns.clear();

    QueryResult result = CharacterDatabase.PQuery("SELECT spell,time FROM pet_spell_cooldown WHERE guid = '%u'",m_charmInfo->GetPetNumber());

    if(result)
    {
        time_t curTime = time(nullptr);

        WorldPacket data(SMSG_SPELL_COOLDOWN, (8+1+result->GetRowCount()*8));
        data << GetGUID();
        data << uint8(0x0);                                 // flags (0x1, 0x2)

        do
        {
            Field *fields = result->Fetch();

            uint32 spell_id = fields[0].GetUInt32();
            time_t db_time  = (time_t)fields[1].GetUInt64();

            if(!sSpellMgr->GetSpellInfo(spell_id))
            {
                TC_LOG_ERROR("entities.pet","Pet %u have unknown spell %u in `pet_spell_cooldown`, skipping.",m_charmInfo->GetPetNumber(),spell_id);
                continue;
            }

            // skip outdated cooldown
            if(db_time <= curTime)
                continue;

            data << uint32(spell_id);
            data << uint32(uint32(db_time-curTime)*1000);   // in m.secs

            _AddCreatureSpellCooldown(spell_id,db_time);
        }
        while( result->NextRow() );

        if(!m_CreatureSpellCooldowns.empty() && GetOwner())
        {
            (GetOwner()->ToPlayer())->SendDirectMessage(&data);
        }
    }
}

void Pet::_SaveSpellCooldowns()
{
    SQLTransaction trans = CharacterDatabase.BeginTransaction();
    trans->PAppend("DELETE FROM pet_spell_cooldown WHERE guid = '%u'", m_charmInfo->GetPetNumber());

    time_t curTime = time(nullptr);

    // remove oudated and save active
    for(auto itr = m_CreatureSpellCooldowns.begin();itr != m_CreatureSpellCooldowns.end();)
    {
        if(itr->second <= curTime)
            m_CreatureSpellCooldowns.erase(itr++);
        else
        {
            trans->PAppend("INSERT INTO pet_spell_cooldown (guid,spell,time) VALUES ('%u', '%u', '" UI64FMTD "')", m_charmInfo->GetPetNumber(), itr->first, uint64(itr->second));
            ++itr;
        }
    }
    CharacterDatabase.CommitTransaction(trans);
}

void Pet::_LoadSpells()
{
    QueryResult result = CharacterDatabase.PQuery("SELECT spell,slot,active FROM pet_spell WHERE guid = '%u'",m_charmInfo->GetPetNumber());

    if(result)
    {
        do
        {
            Field *fields = result->Fetch();

            AddSpell(fields[0].GetUInt16(), fields[2].GetUInt8(), PETSPELL_UNCHANGED, fields[1].GetUInt16());
        }
        while( result->NextRow() );
    }
}

void Pet::_SaveSpells()
{
    for (PetSpellMap::const_iterator itr = m_spells.begin(), next = m_spells.begin(); itr != m_spells.end(); itr = next)
    {
        ++next;
        if (itr->second->type == PETSPELL_FAMILY) continue; // prevent saving family passives to DB
        SQLTransaction trans = CharacterDatabase.BeginTransaction();
        if (itr->second->state == PETSPELL_REMOVED || itr->second->state == PETSPELL_CHANGED)
            trans->PAppend("DELETE FROM pet_spell WHERE guid = '%u' and spell = '%u'", m_charmInfo->GetPetNumber(), itr->first);
        if (itr->second->state == PETSPELL_NEW || itr->second->state == PETSPELL_CHANGED)
            trans->PAppend("INSERT INTO pet_spell (guid,spell,slot,active) VALUES ('%u', '%u', '%u','%u')", m_charmInfo->GetPetNumber(), itr->first, itr->second->slotId,itr->second->active);
        CharacterDatabase.CommitTransaction(trans);

        if (itr->second->state == PETSPELL_REMOVED)
            _removeSpell(itr->first);
        else
            itr->second->state = PETSPELL_UNCHANGED;
    }
}

void Pet::_LoadAuras(uint32 timediff)
{
    m_Auras.clear();
    for (auto & m_modAura : m_modAuras)
        m_modAura.clear();

    // all aura related fields
    for(int i = UNIT_FIELD_AURA; i <= UNIT_FIELD_AURASTATE; ++i)
        SetUInt32Value(i, 0);

    QueryResult result = CharacterDatabase.PQuery("SELECT caster_guid,spell,effect_index,stackcount,amount,maxduration,remaintime,remaincharges FROM pet_aura WHERE guid = '%u'",m_charmInfo->GetPetNumber());

    if(result)
    {
        do
        {
            Field *fields = result->Fetch();
            uint64 caster_guid = fields[0].GetUInt64();
            uint32 spellid = fields[1].GetUInt32();
            uint32 effindex = fields[2].GetUInt32();
            uint32 stackcount= fields[3].GetUInt32();
            int32 damage     = (int32)fields[4].GetUInt32();
            int32 maxduration = (int32)fields[5].GetUInt32();
            int32 remaintime = (int32)fields[6].GetUInt32();
            int32 remaincharges = (int32)fields[7].GetUInt32();

            SpellInfo const* spellproto = sSpellMgr->GetSpellInfo(spellid);
            if(!spellproto)
            {
                TC_LOG_ERROR("entities.pet","Unknown aura (spellid %u, effindex %u), ignore.",spellid,effindex);
                continue;
            }

            if(effindex >= 3)
            {
                TC_LOG_ERROR("entities.pet","Invalid effect index (spellid %u, effindex %u), ignore.",spellid,effindex);
                continue;
            }

            // negative effects should continue counting down after logout
            if (remaintime != -1 && !spellproto->IsPositiveEffect(effindex))
            {
                if(remaintime  <= int32(timediff))
                    continue;

                remaintime -= timediff;
            }

            // prevent wrong values of remaincharges
            if(spellproto->ProcCharges)
            {
                if(remaincharges <= 0 || remaincharges > spellproto->ProcCharges)
                    remaincharges = spellproto->ProcCharges;
            }
            else
                remaincharges = -1;

            /// do not load single target auras (unless they were cast by the player)
            if (caster_guid != GetGUID() && spellproto->IsSingleTarget())
                continue;
                
            bool abort = false;
            for (const auto & Effect : spellproto->Effects) { // Don't load these, they make the core crash sometimes
                if (Effect.ApplyAuraName == SPELL_AURA_IGNORED)
                    abort = true;
            }

            Unit* owner = GetOwner(); 
            // load negative auras only if player has recently dismissed his pet
            if(owner && !owner->HasAuraEffect(SPELL_PET_RECENTLY_DISMISSED) && !spellproto->IsPositiveEffect(effindex))
                continue;

            if (abort)
                continue;

            for(uint32 i=0; i<stackcount; i++)
            {
                Aura* aura = CreateAura(spellproto, effindex, nullptr, this, nullptr);

                if(!damage)
                    damage = aura->GetModifier()->m_amount;
                aura->SetLoadedState(caster_guid,damage,maxduration,remaintime,remaincharges);
                AddAura(aura);
            }
        }
        while( result->NextRow() );
    }
}

void Pet::_SaveAuras()
{
    SQLTransaction trans = CharacterDatabase.BeginTransaction();
    trans->PAppend("DELETE FROM pet_aura WHERE guid = '%u'", m_charmInfo->GetPetNumber());

    AuraMap const& auras = GetAuras();
    if (auras.empty())
    {
        CharacterDatabase.CommitTransaction(trans);
        return;
    }
    
    for(const auto & itr : auras)
    {
        Aura* aura = itr.second;
        SpellInfo const *spellInfo = aura->GetSpellInfo();

        //skip if passive
        if(aura->IsPassive())
            continue;

        // skip all auras from spell that apply at cast SPELL_AURA_MOD_SHAPESHIFT or pet area auras.
        for (const auto & Effect : spellInfo->Effects)
        {
            if (Effect.ApplyAuraName == SPELL_AURA_MOD_STEALTH ||
                Effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_OWNER ||
                Effect.Effect == SPELL_EFFECT_APPLY_AREA_AURA_PET )
                continue;
        }

        // don't save guid of caster in case we are caster of the spell - guid for pet is generated every pet load, so it won't match saved guid anyways
        uint64 casterGUID = (aura->GetCasterGUID() == GetGUID()) ? 0 : aura->GetCasterGUID();

        trans->PAppend("INSERT INTO pet_aura (guid,caster_guid,spell,effect_index,stackcount,amount,maxduration,remaintime,remaincharges)"
            "VALUES ('%u', '" UI64FMTD "', '%u', '%u', '%u', '%d', '%d', '%d', '%d')",
            m_charmInfo->GetPetNumber(), casterGUID,(uint32)aura->GetId(), (uint32)aura->GetEffIndex(),
            (uint32)aura->GetStackAmount(), aura->GetModifier()->m_amount,int(aura->GetAuraMaxDuration()),
            int(aura->GetAuraDuration()),int(aura->m_procCharges));
    }

    CharacterDatabase.CommitTransaction(trans);
}

bool Pet::AddSpell(uint16 spell_id, uint16 active, PetSpellState state, uint16 slot_id, PetSpellType type)
{
    SpellInfo const *spellInfo = sSpellMgr->GetSpellInfo(spell_id);
    if (!spellInfo)
    {
        // do pet spell book cleanup
        if(state == PETSPELL_UNCHANGED)                     // spell load case
        {
            TC_LOG_ERROR("entities.pet","Pet::addSpell: Non-existed in SpellStore spell #%u request, deleting for all pets in `pet_spell`.",spell_id);
            CharacterDatabase.PExecute("DELETE FROM pet_spell WHERE spell = '%u'",spell_id);
        }
        else
            TC_LOG_ERROR("entities.pet","Pet::addSpell: Non-existed in SpellStore spell #%u request.",spell_id);

        return false;
    }

    // same spells don't have autocast option
    if (spellInfo->HasAttribute(SPELL_ATTR1_UNAUTOCASTABLE_BY_PET))
        active = ACT_CAST;

    auto itr = m_spells.find(spell_id);
    if (itr != m_spells.end())
    {
        if (itr->second->state == PETSPELL_REMOVED)
        {
            delete itr->second;
            m_spells.erase(itr);
            state = PETSPELL_CHANGED;
        }
        else if (state == PETSPELL_UNCHANGED && itr->second->state != PETSPELL_UNCHANGED)
        {
            // can be in case spell loading but learned at some previous spell loading
            itr->second->state = PETSPELL_UNCHANGED;
            return false;
        }
        else
            return false;
    }

    uint32 oldspell_id = 0;

    auto newspell = new PetSpell;
    newspell->state = state;
    newspell->type = type;
    
    if(active == ACT_DECIDE)                                //active was not used before, so we save it's autocast/passive state here
    {
        if(spellInfo && spellInfo->IsPassive())
            newspell->active = ACT_PASSIVE;
        else
            newspell->active = ACT_DISABLED;
    }
    else
        newspell->active = active;

    uint32 chainstart = sSpellMgr->GetFirstSpellInChain(spell_id);

    for (auto & m_spell : m_spells)
    {
        if(m_spell.second->state == PETSPELL_REMOVED) continue;

        if(sSpellMgr->GetFirstSpellInChain(m_spell.first) == chainstart)
        {
            slot_id = m_spell.second->slotId;
            newspell->active = m_spell.second->active;

            if(newspell->active == ACT_ENABLED)
                ToggleAutocast(m_spell.first, false);

            oldspell_id = m_spell.first;
            RemoveSpell(m_spell.first);
        }
    }

    uint16 tmpslot=slot_id;

    if (tmpslot == 0xffff)
    {
        uint16 maxid = 0;
        PetSpellMap::iterator itr;
        for (itr = m_spells.begin(); itr != m_spells.end(); ++itr)
        {
            if(itr->second->state == PETSPELL_REMOVED) continue;
            if (itr->second->slotId > maxid) maxid = itr->second->slotId;
        }
        tmpslot = maxid + 1;
    }

    newspell->slotId = tmpslot;
    m_spells[spell_id] = newspell;

    if (spellInfo->IsPassive())
        CastSpell(this, spell_id, true);
    else if(state == PETSPELL_NEW)
    {
        for (uint8 i = 0; i < CREATURE_MAX_SPELLS; i++)
        {
            if (m_charmInfo->AddSpellToAB(oldspell_id, spell_id, i, (ActiveStates)active))
                break;
        }
    }

    if(newspell->active == ACT_ENABLED)
        ToggleAutocast(spell_id, true);

    return true;
}

bool Pet::LearnSpell(uint16 spell_id)
{
    // prevent duplicated entires in spell book
    if (!AddSpell(spell_id))
        return false;

    Unit* owner = GetOwner();
    if(owner->GetTypeId()==TYPEID_PLAYER)
        (owner->ToPlayer())->PetSpellInitialize();
    return true;
}

void Pet::RemoveSpell(uint16 spell_id)
{
    auto itr = m_spells.find(spell_id);
    if (itr == m_spells.end())
        return;

    if(itr->second->state == PETSPELL_REMOVED)
        return;

    if(itr->second->state == PETSPELL_NEW)
    {
        delete itr->second;
        m_spells.erase(itr);
    }
    else
        itr->second->state = PETSPELL_REMOVED;

    RemoveAurasDueToSpell(spell_id);
}

bool Pet::_removeSpell(uint16 spell_id)
{
    auto itr = m_spells.find(spell_id);
    if (itr != m_spells.end())
    {
        delete itr->second;
        m_spells.erase(itr);
        return true;
    }
    return false;
}

void Pet::InitPetCreateSpells()
{
    m_charmInfo->InitPetActionBar();

    m_spells.clear();
    int32 usedtrainpoints = 0, petspellid;
    PetCreateSpellEntry const* CreateSpells = sObjectMgr->GetPetCreateSpellEntry(GetEntry());
    if(CreateSpells)
    {
        for(uint32 i : CreateSpells->spellid)
        {
            if(!i)
                break;

            SpellInfo const *learn_spellproto = sSpellMgr->GetSpellInfo(i);
            if(!learn_spellproto)
                continue;

            if(learn_spellproto->Effects[0].Effect == SPELL_EFFECT_LEARN_SPELL || learn_spellproto->Effects[0].Effect == SPELL_EFFECT_LEARN_PET_SPELL)
            {
                petspellid = learn_spellproto->Effects[0].TriggerSpell;
                Unit* owner = GetOwner();
                if(owner && owner->GetTypeId() == TYPEID_PLAYER && !(owner->ToPlayer())->HasSpell(learn_spellproto->Id))
                {
                    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(petspellid);
                    if(spellInfo->IsPassive())          //learn passive skills when tamed, not sure if thats right
                        (owner->ToPlayer())->LearnSpell(learn_spellproto->Id, false);
                    else
                        AddTeachSpell(learn_spellproto->Effects[0].TriggerSpell, learn_spellproto->Id);
                }
            }
            else
                petspellid = learn_spellproto->Id;

            if (petspellid == SPELL_WATER_ELEMENTAL_WATERBOLT)
                AddSpell(petspellid,ACT_ENABLED);
            else
                AddSpell(petspellid);

            SkillLineAbilityMapBounds skill_bounds = sSpellMgr->GetSkillLineAbilityMapBounds(learn_spellproto->Effects[0].TriggerSpell);
            for(auto _spell_idx = skill_bounds.first; _spell_idx != skill_bounds.second; ++_spell_idx)
            {
                usedtrainpoints += _spell_idx->second->reqtrainpoints;
                break;
            }
        }
    }

    LearnPetPassives();

    CastPetAuras(false);

    SetTP(-usedtrainpoints);
}

void Pet::CheckLearning(uint32 spellid)
{
                                                            //charmed case -> prevent crash
    if(GetTypeId() == TYPEID_PLAYER || getPetType() != HUNTER_PET)
        return;

    Unit* owner = GetOwner();

    if(m_teachspells.empty() || !owner || owner->GetTypeId() != TYPEID_PLAYER)
        return;

    auto itr = m_teachspells.find(spellid);
    if(itr == m_teachspells.end())
        return;

    if(urand(0, 100) < 10)
    {
        (owner->ToPlayer())->LearnSpell(itr->second, false);
        m_teachspells.erase(itr);
    }
}

uint32 Pet::ResetTalentsCost() const
{
    uint32 days = (sWorld->GetGameTime() - m_resetTalentsTime)/DAY;

    // The first time reset costs 10 silver; after 1 day cost is reset to 10 silver
    if(m_resetTalentsCost < 10*SILVER || days > 0)
        return 10*SILVER;
    // then 50 silver
    else if(m_resetTalentsCost < 50*SILVER)
        return 50*SILVER;
    // then 1 gold
    else if(m_resetTalentsCost < 1*GOLD)
        return 1*GOLD;
    // then increasing at a rate of 1 gold; cap 10 gold
    else
        return (m_resetTalentsCost + 1*GOLD > 10*GOLD ? 10*GOLD : m_resetTalentsCost + 1*GOLD);
}

void Pet::ToggleAutocast(uint32 spellid, bool apply)
{
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellid);
    if(!spellInfo)
        return;

    if(spellInfo->IsPassive())
        return;

    PetSpellMap::const_iterator itr = m_spells.find((uint16)spellid);

    int i;

    if(apply)
    {
        for (i = 0; i < m_autospells.size() && m_autospells[i] != spellid; i++);
        if (i == m_autospells.size())
        {
            m_autospells.push_back(spellid);
            itr->second->active = ACT_ENABLED;
            itr->second->state = PETSPELL_CHANGED;
        }
    }
    else
    {
        auto itr2 = m_autospells.begin();
        for (i = 0; i < m_autospells.size() && m_autospells[i] != spellid; i++, itr2++);
        if (i < m_autospells.size())
        {
            m_autospells.erase(itr2);
            itr->second->active = ACT_DISABLED;
            itr->second->state = PETSPELL_CHANGED;
        }
    }
}

bool Pet::Create(uint32 guidlow, Map *map, uint32 Entry, uint32 pet_number)
{
    SetMapId(map->GetId());
    SetInstanceId(map->GetInstanceId());

    Object::_Create(guidlow, pet_number, HIGHGUID_PET);

    m_DBTableGuid = guidlow;
    m_originalEntry = Entry;

    if(!InitEntry(Entry))
        return false;

    SetByteValue(UNIT_FIELD_BYTES_2, 0, SHEATH_STATE_MELEE );
    //SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3 | UNIT_BYTE2_FLAG_AURAS | UNIT_BYTE2_FLAG_UNK5 );
    SetByteValue(UNIT_FIELD_BYTES_2, 1, UNIT_BYTE2_FLAG_UNK3);

    if(getPetType() == MINI_PET)                            // always non-attackable
        SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NON_ATTACKABLE);

    InitPetAuras(Entry);

    return true;
}

void Pet::InitPetAuras(const uint32 Entry)
{
    CreatureTemplate const *cInfo = sObjectMgr->GetCreatureTemplate(Entry);
    if(!cInfo)
        return;

    uint32 aura1 = 0, aura2 = 0, aura3 = 0;

    switch(cInfo->family)
    {
        // WARLOCK PETS:
        case CREATURE_FAMILY_IMP:
            aura1 = 18728;
            aura2 = 18737;
            aura3 = 18740;
            break;
        case CREATURE_FAMILY_FELHUNTER:
            aura1 = 18730;
            aura2 = 18738;
            aura3 = 18739;
            break;
        case CREATURE_FAMILY_VOIDWALKER:
            aura1 = 18727;
            aura2 = 18735;
            aura3 = 18742;
            break;
        case CREATURE_FAMILY_SUCCUBUS:
            aura1 = 18729;
            aura2 = 18736;
            aura3 = 18741;
            break;
        case CREATURE_FAMILY_FELGUARD:
            aura1 = 30147;
            aura2 = 30148;
            aura3 = 30149;
            break;

        // HUNTER PETS:
        case CREATURE_FAMILY_HYENA:
            aura1 = 17215;
            break;
        case CREATURE_FAMILY_BEAR:
            aura1 = 17208;
            break;
        case CREATURE_FAMILY_SERPENT:
            aura1 = 35386;
            break;
        case CREATURE_FAMILY_WOLF:
            aura1 = 17223;
            break;
        case CREATURE_FAMILY_WARP_STALKER:
            aura1 = 35254;
            break;
        case CREATURE_FAMILY_SPOREBAT:
            aura1 = 35258;
            break;
        case CREATURE_FAMILY_DRAGONHAWK:
            aura1 = 34887;
            break;
        case CREATURE_FAMILY_NETHER_RAY:
            aura1 = 35253;
            break;
        case CREATURE_FAMILY_RAVAGER:
            aura1 = 35257;
            break;
        case CREATURE_FAMILY_BOAR:
            aura1 = 7000;
            break;
        case CREATURE_FAMILY_BAT:
            aura1 = 17206;
            break;
        case CREATURE_FAMILY_CARRION_BIRD:
            aura1 = 17209;
            break;
        case CREATURE_FAMILY_CAT:
            aura1 = 17210;
            break;
        case CREATURE_FAMILY_SPIDER:
            aura1 = 17219;
            break;
        case CREATURE_FAMILY_CROCOLISK:
            aura1 = 17212;
            break;
        case CREATURE_FAMILY_CRAB:
            aura1 = 17211;
            break;
        case CREATURE_FAMILY_GORILLA:
            aura1 = 17214;
            break;
        case CREATURE_FAMILY_RAPTOR:
            aura1 = 17217;
            break;
        case CREATURE_FAMILY_TALLSTRIDER:
            aura1 = 17220;
            break;
        case CREATURE_FAMILY_SCORPID:
            aura1 = 17218;
            break;
        case CREATURE_FAMILY_TURTLE:
            aura1 = 17221;
            break;
        case CREATURE_FAMILY_OWL:
            aura1 = 17216;
            break;
        case CREATURE_FAMILY_WIND_SERPENT:
            aura1 = 17222;
            break;
        case CREATURE_FAMILY_DOOMGUARD:
        case CREATURE_FAMILY_REMOTE_CONTROL:
        case CREATURE_FAMILY_SEA_LION:
        default:
            return;
    }

    if(aura1)
        CastSpell(this, aura1, true);
    if(aura2)
        CastSpell(this, aura2, true);
    if(aura3)
        CastSpell(this, aura3, true);

    // Hunter Pets have multiple auras
    if(getPetType() == HUNTER_PET)
    {
        CastSpell(this, 8875, true);    // Damage
        CastSpell(this, 19580, true);   // Armor
        CastSpell(this, 19581, true);   // HP
        CastSpell(this, 19582, true);   // Speed
        CastSpell(this, 19589, true);   // Power Regen
        CastSpell(this, 19591, true);   // Critical Chance
        CastSpell(this, 20784, true);   // Frenzy Chance
        CastSpell(this, 34666, true);   // Hit Chance
        CastSpell(this, 34667, true);   // Dodge Chance
        CastSpell(this, 34675, true);   // Attack Speed
    }
}

bool Pet::HasSpell(uint32 spell) const
{
    return (m_spells.find(spell) != m_spells.end());
}

// Get all passive spells in our skill line
void Pet::LearnPetPassives()
{
    CreatureTemplate const* cInfo = GetCreatureTemplate();
    if(!cInfo)
        return;

    CreatureFamilyEntry const* cFamily = sCreatureFamilyStore.LookupEntry(cInfo->family);
    if(!cFamily)
        return;

    PetFamilySpellsStore::const_iterator petStore = sPetFamilySpellsStore.find(cFamily->ID);
    if(petStore != sPetFamilySpellsStore.end())
    {
        // For general hunter pets skill 270
        // Passive 01~10, Passive 00 (20782, not used), Ferocious Inspiration (34457)
        // Scale 01~03 (34902~34904, bonus from owner, not used)
        for(uint32 petSet : petStore->second)
            AddSpell(petSet, ACT_DECIDE, PETSPELL_NEW, 0xffff, PETSPELL_FAMILY);
    }
}

void Pet::CastPetAuras(bool current)
{
    Unit* owner = GetOwner();
    if(!owner)
        return;

    if(getPetType() != HUNTER_PET && (getPetType() != SUMMON_PET || owner->GetClass() != CLASS_WARLOCK))
        return;

    for(auto itr = owner->m_petAuras.begin(); itr != owner->m_petAuras.end();)
    {
        PetAura const* pa = *itr;
        ++itr;

        if(!current && pa->IsRemovedOnChangePet())
            owner->RemovePetAura(pa);
        else
            CastPetAura(pa);
    }
}

void Pet::CastPetAura(PetAura const* aura)
{
    uint16 auraId = aura->GetAura(GetEntry());
    if(!auraId)
        return;

    if(auraId == 35696)                                       // Demonic Knowledge
    {
        int32 basePoints = int32(aura->GetDamage() * (GetStat(STAT_STAMINA) + GetStat(STAT_INTELLECT)) / 100);
        CastCustomSpell(this, auraId, &basePoints, nullptr, nullptr, true);
    }
    else
        CastSpell(this, auraId, true);
}

