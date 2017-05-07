/* Copyright (C) 2006 - 2008 ScriptDev2 <https://scriptdev2.svn.sourceforge.net/>
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* ScriptData
SDName: Boss_Skeram
SD%Complete: 75
SDComment: Mind Control buggy.
SDCategory: Temple of Ahn'Qiraj
EndScriptData */


#include "def_temple_of_ahnqiraj.h"
#include "Group.h"

#define SAY_AGGRO1                  -1531000
#define SAY_AGGRO2                  -1531001
#define SAY_AGGRO3                  -1531002
#define SAY_SLAY1                   -1531003
#define SAY_SLAY2                   -1531004
#define SAY_SLAY3                   -1531005
#define SAY_SPLIT                   -1531006
#define SAY_DEATH                   -1531007

#define SPELL_ARCANE_EXPLOSION      25679
#define SPELL_EARTH_SHOCK           26194
#define SPELL_TRUE_FULFILLMENT4     26526
#define SPELL_BLINK                 28391

#define PLACES_CLEANUP delete place1; \
  delete place2;                      \
  delete place3;                      \

class ov_mycoordinates
{
    public:
        float x,y,z,r;
        ov_mycoordinates(float cx, float cy, float cz, float cr)
        {
            x = cx; y = cy; z = cz; r = cr;
        }
};

struct boss_skeramAI : public ScriptedAI
{
    boss_skeramAI(Creature *c) : ScriptedAI(c)
    {
        pInstance = ((InstanceScript*)c->GetInstanceScript());
        IsImage = false;
    }

    InstanceScript *pInstance;

    uint32 ArcaneExplosion_Timer;
    uint32 EarthShock_Timer;
    uint32 FullFillment_Timer;
    uint32 Blink_Timer;
    uint32 Invisible_Timer;

    Creature *Image1, *Image2;

    bool Images75;
    bool Images50;
    bool Images25;
    bool IsImage;
    bool Invisible;

    void Reset()
    override {
        ArcaneExplosion_Timer = 6000 + rand()%6000;
        EarthShock_Timer = 2000;
        FullFillment_Timer = 15000;
        Blink_Timer = 8000 + rand()%12000;
        Invisible_Timer = 500;

        Images75 = false;
        Images50 = false;
        Images25 = false;
        Invisible = false;

        me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
        me->SetVisibility(VISIBILITY_ON);

        if (IsImage)
            me->SetDeathState(JUST_DIED);
    }

    void KilledUnit(Unit* victim)
    override {
        switch(rand()%3)
        {
        case 0: DoScriptText(SAY_SLAY1, me); break;
        case 1: DoScriptText(SAY_SLAY2, me); break;
        case 2: DoScriptText(SAY_SLAY3, me); break;
        }
    }

    void JustDied(Unit* Killer)
    override {
        if (!IsImage)
            DoScriptText(SAY_DEATH, me);
    }

    void EnterCombat(Unit *who)
    override {
        if (IsImage || Images75)
            return;
        switch(rand()%3)
        {
        case 0: DoScriptText(SAY_AGGRO1, me); break;
        case 1: DoScriptText(SAY_AGGRO2, me); break;
        case 2: DoScriptText(SAY_AGGRO3, me); break;
        }
    }

    void UpdateAI(const uint32 diff)
    override {
        //Return since we have no target
        if (!UpdateVictim())
            return;

        //ArcaneExplosion_Timer
        if (ArcaneExplosion_Timer < diff)
        {
            DoCast(me->GetVictim(), SPELL_ARCANE_EXPLOSION);
            ArcaneExplosion_Timer = 8000 + rand()%10000;
        }else ArcaneExplosion_Timer -= diff;

        //If we are within range melee the target
        if( me->IsWithinMeleeRange(me->GetVictim()))
        {
            //Make sure our attack is ready and we arn't currently casting
            if( me->IsAttackReady() && !me->IsNonMeleeSpellCast(false))
            {
                me->AttackerStateUpdate(me->GetVictim());
                me->ResetAttackTimer();
            }
        }else
        {
            //EarthShock_Timer
            if (EarthShock_Timer < diff)
            {
                DoCast(me->GetVictim(),SPELL_EARTH_SHOCK);
                EarthShock_Timer = 1000;
            }else EarthShock_Timer -= diff;
        }

        //Blink_Timer
        if (Blink_Timer < diff)
        {
            //DoCast(me, SPELL_BLINK);
            switch(rand()%3)
            {
                case 0:
                    me->Relocate(-8340.782227,2083.814453,125.648788,0);
                    DoResetThreat();
                    break;
                case 1:
                    me->Relocate(-8341.546875,2118.504639,133.058151,0);
                    DoResetThreat();
                    break;
                case 2:
                    me->Relocate(-8318.822266,2058.231201,133.058151,0);
                    DoResetThreat();
                    break;
            }
            DoStopAttack();

            Blink_Timer= 20000 + rand()%20000;
        }else Blink_Timer -= diff;

        int procent = (int) (me->GetHealthPct() +0.5);

        //Summoning 2 Images and teleporting to a random position on 75% health
        if ( (!Images75 && !IsImage) && (procent <= 75 && procent > 70) )
            DoSplit(75);

        //Summoning 2 Images and teleporting to a random position on 50% health
        if ( (!Images50 && !IsImage) &&
            (procent <= 50 && procent > 45) )
            DoSplit(50);

        //Summoning 2 Images and teleporting to a random position on 25% health
        if ( (!Images25 && !IsImage) && (procent <= 25 && procent > 20) )
            DoSplit(25);

        //Invisible_Timer
        if (Invisible)
        {
            if (Invisible_Timer < diff)
            {
                //Making Skeram visible after telporting
                me->SetVisibility(VISIBILITY_ON);
                me->RemoveFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);

                Invisible_Timer = 2500;
                Invisible = false;
            }else Invisible_Timer -= diff;
        }

        DoMeleeAttackIfReady();
    }

    void DoSplit(int atPercent /* 75 50 25 */)
    {
        DoScriptText(SAY_SPLIT, me);

        auto place1 = new ov_mycoordinates(-8340.782227,2083.814453,125.648788,0);
        auto place2 = new ov_mycoordinates(-8341.546875,2118.504639,133.058151,0);
        auto place3 = new ov_mycoordinates(-8318.822266,2058.231201,133.058151,0);

        ov_mycoordinates *bossc=place1, *i1=place2, *i2=place3;

        switch(rand()%3)
        {
            case 0:
                bossc=place1;
                i1=place2;
                i2=place3;
                break;
            case 1:
                bossc=place2;
                i1=place1;
                i2=place3;
                break;
            case 2:
                bossc=place3;
                i1=place1;
                i2=place2;
                break;
        }

        for (int tryi = 0; tryi < 41; tryi ++)
        {
            Unit *targetpl = SelectTarget(SELECT_TARGET_RANDOM, 0);
            if (targetpl->GetTypeId() == TYPEID_PLAYER)
            {
                Group *grp = (targetpl->ToPlayer())->GetGroup();
                if (grp)
                {
                    for (int ici = 0; ici < TARGETICONCOUNT; ici++)
                    {
                        //if (grp ->m_targetIcons[ici] == me->GetGUID()) -- private member:(
                        grp->SetTargetIcon(ici, 0);
                    }
                }
                break;
            }
        }

        me->RemoveAllAuras();
        me->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_SELECTABLE);
        me->SetVisibility(VISIBILITY_OFF);
        me->Relocate(bossc->x, bossc->y, bossc->z, bossc->r);
        Invisible = true;
        DoResetThreat();
        DoStopAttack();

        switch (atPercent)
        {
            case 75: Images75 = true; break;
            case 50: Images50 = true; break;
            case 25: Images25 = true; break;
        }

        Unit* target = SelectTarget(SELECT_TARGET_RANDOM,0);

        Image1 = me->SummonCreature(15263, i1->x, i1->y, i1->z, i1->r, TEMPSUMMON_CORPSE_DESPAWN, 30000);
        if(!Image1)
        {
          PLACES_CLEANUP
          return;
        }
        Image1->SetMaxHealth(me->GetMaxHealth() / 5);
        Image1->SetHealth(me->GetHealth() / 5);
        if (target)
            Image1->AI()->AttackStart(target);

        Image2 = me->SummonCreature(15263,i2->x, i2->y, i2->z, i2->r, TEMPSUMMON_CORPSE_DESPAWN, 30000);
        if(!Image2)
        {
          PLACES_CLEANUP
          return;
        }
        Image2->SetMaxHealth(me->GetMaxHealth() / 5);
        Image2->SetHealth(me->GetHealth() / 5);
        if (target)
            Image2->AI()->AttackStart(target);

        ((boss_skeramAI*)Image1->AI())->IsImage = true;
        ((boss_skeramAI*)Image2->AI())->IsImage = true;

        Invisible = true;
    PLACES_CLEANUP
    }

};

CreatureAI* GetAI_boss_skeram(Creature *_Creature)
{
    return new boss_skeramAI (_Creature);
}

void AddSC_boss_skeram()
{
    OLDScript *newscript;
    newscript = new OLDScript;
    newscript->Name="boss_skeram";
    newscript->GetAI = &GetAI_boss_skeram;
    sScriptMgr->RegisterOLDScript(newscript);
}

