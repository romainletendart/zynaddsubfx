/*
  ZynAddSubFX - a software synthesizer

  Part.cpp - Part implementation
  Copyright (C) 2002-2005 Nasca Octavian Paul
  Author: Nasca Octavian Paul

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License
  as published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License (version 2 or later) for more details.

  You should have received a copy of the GNU General Public License (version 2)
  along with this program; if not, write to the Free Software Foundation,
  Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA

*/

#include "Part.h"
#include "Microtonal.h"
#include "Util.h"
#include "XMLwrapper.h"
#include "../Effects/EffectMgr.h"
#include "../Params/ADnoteParameters.h"
#include "../Params/SUBnoteParameters.h"
#include "../Params/PADnoteParameters.h"
#include "../Synth/ADnote.h"
#include "../Synth/SUBnote.h"
#include "../Synth/PADnote.h"
#include "../DSP/FFTwrapper.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <cassert>

#include <rtosc/ports.h>
#include <rtosc/port-sugar.h>

using rtosc::Ports;
using rtosc::RtData;

#define rObject Part
static Ports partPorts = {
    rRecurs(kit, 16, "Kit"),//NUM_KIT_ITEMS
    rRecursp(partefx, 3, "Part Effect"),
    rRecur(ctl,       "Controller"),
    rToggle(Penabled, "Part enable"),
    rParam(Pvolume, "Part Volume"),
    rParam(Pminkey, "Min Used Key"),
    rParam(Pmaxkey, "Max Used Key"),
    rParam(Pkeyshift, "Part keyshift"),
    rParam(Prcvchn,  "Active MIDI channel"),
    rParam(Ppanning, "Set Panning"),
    rParam(Pvelsns,   "Velocity sensing"),
    rParam(Pveloffs,  "Velocity offset"),
    rToggle(Pnoteon,  "If the channel accepts note on events"),
    rToggle(Pdrummode, "Drum mode enable"),
    rToggle(Ppolymode,  "Polyphoney mode"),
    rToggle(Plegatomode, "Legato enable"),
    rParam(Pkeylimit,   "Key limit per part"),
    rParam(info.Ptype, "Class of Instrument"),
    rString(info.Pauthor, MAX_INFO_TEXT_SIZE, "Instrument Author"),
    rString(info.Pcomments, MAX_INFO_TEXT_SIZE, "Instrument Comments"),
    {"captureMin:", NULL, NULL, [](const char *, RtData &r)
        {Part *p = (Part*)r.obj; p->Pminkey = p->lastnote;}},
    {"captureMax:", NULL, NULL, [](const char *, RtData &r)
        {Part *p = (Part*)r.obj; p->Pmaxkey = p->lastnote;}},
    {"polyType::c:i", NULL, NULL, [](const char *msg, RtData &d)
        {
            Part *p = (Part*)d.obj;
            if(!rtosc_narguments(msg)) {
                int res = 0;
                if(!p->Ppolymode)
                    res = p->Plegatomode ? 2 : 1;
                d.reply(d.loc, "c", res);
                return;
            }

            int i = rtosc_argument(msg, 0).i;
            if(i == 0) {
                p->Ppolymode = 1;
                p->Plegatomode = 0;
            } else if(i==1) {
                p->Ppolymode = 0;
                p->Plegatomode = 0;
            } else {
                p->Ppolymode = 0;
                p->Plegatomode = 1;
            }}},

    //{"kit#16::T:F", "::Enables or disables kit item", 0,
    //    [](const char *m, RtData &d) {
    //        Part *p = (Part*)d.obj;
    //        unsigned kitid = -1;
    //        //Note that this event will be captured before transmitted, thus
    //        //reply/broadcast don't matter
    //        for(int i=0; i<VOICES; ++i) {
    //            d.reply("/middleware/oscil", "siisb", loc, kitid, i, "oscil"
    //                    sizeof(OscilGen*),
    //                    p->kit[kitid]->adpars->voice[i]->OscilSmp);
    //            d.reply("/middleware/oscil", "siisb", loc, kitid, i, "oscil-mod"
    //                    sizeof(OscilGen*),
    //                    p->kit[kitid]->adpars->voice[i]->somethingelse);
    //        }
    //        d.reply("/middleware/pad", "sib", loc, kitid,
    //                sizeof(PADnoteParameters*),
    //                p->kit[kitid]->padpars)
    //    }}
};

#undef  rObject
#define rObject Part::Kit
static Ports kitPorts = {
    rRecurp(padpars, "Padnote parameters"),
    rRecurp(adpars, "Adnote parameters"),
    rRecurp(subpars, "Adnote parameters"),
    rToggle(Penabled, "Kit item enable"),
    rToggle(Pmuted,   "Kit item mute"),
    rParam(Pminkey,   "Kit item min key"),
    rParam(Pmaxkey,   "Kit item max key"),
    rToggle(Padenabled, "ADsynth enable"),
    rToggle(Psubenabled, "SUBsynth enable"),
    rToggle(Ppadenabled, "PADsynth enable"),
    rParam(Psendtoparteffect, "Effect Levels"),
    rString(Pname, PART_MAX_NAME_LEN, "Kit User Specified Label"),
    //{"padpars:b", "::", 0
    //    [](
};

Ports &Part::Kit::ports = kitPorts;
Ports &Part::ports = partPorts;

Part::Part(Microtonal *microtonal_, FFTwrapper *fft_)
{
    microtonal = microtonal_;
    fft      = fft_;
    pthread_mutex_init(&load_mutex, NULL);
    partoutl = new float [synth->buffersize];
    partoutr = new float [synth->buffersize];

    for(int n = 0; n < NUM_KIT_ITEMS; ++n) {
        kit[n].Pname   = new char [PART_MAX_NAME_LEN];
        //XXX this is wasting memory, but making interfacing with the back
        //layers more nice, if this seems to increase memory usage figure out a
        //sane way of tracking the changing pointers (otherwise enjoy the bloat)
        kit[n].adpars  = new ADnoteParameters(fft);
        kit[n].subpars = new SUBnoteParameters();
        kit[n].padpars = new PADnoteParameters(fft);
    }

    //Part's Insertion Effects init
    for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx) {
        partefx[nefx]    = new EffectMgr(1);
        Pefxbypass[nefx] = false;
    }

    for(int n = 0; n < NUM_PART_EFX + 1; ++n) {
        partfxinputl[n] = new float [synth->buffersize];
        partfxinputr[n] = new float [synth->buffersize];
    }

    killallnotes = 0;
    oldfreq      = -1.0f;


    for(int i = 0; i < POLIPHONY; ++i) {
        partnote[i].status = KEY_OFF;
        partnote[i].note   = -1;
        partnote[i].itemsplaying = 0;
        for(int j = 0; j < NUM_KIT_ITEMS; ++j) {
            partnote[i].kititem[j].adnote  = NULL;
            partnote[i].kititem[j].subnote = NULL;
            partnote[i].kititem[j].padnote = NULL;
        }
        partnote[i].time = 0;
    }
    cleanup();

    Pname = new unsigned char [PART_MAX_NAME_LEN];

    oldvolumel = oldvolumer = 0.5f;
    lastnote   = -1;
    lastpos    = 0; // lastpos will store previously used NoteOn(...)'s pos.
    lastlegatomodevalid = false; // To store previous legatomodevalid value.

    defaults();
}
        
void Part::cloneTraits(Part &p) const
{
#define CLONE(x) p.x = this->x
    CLONE(Penabled);

    p.setPvolume(this->Pvolume);
    p.setPpanning(this->Ppanning);

    CLONE(Pminkey);
    CLONE(Pmaxkey);
    CLONE(Pkeyshift);
    CLONE(Prcvchn);

    CLONE(Pvelsns);
    CLONE(Pveloffs);

    CLONE(Pnoteon);
    CLONE(Ppolymode);
    CLONE(Plegatomode);
    CLONE(Pkeylimit);

    CLONE(ctl);
}

void Part::defaults()
{
    Penabled    = 0;
    Pminkey     = 0;
    Pmaxkey     = 127;
    Pnoteon     = 1;
    Ppolymode   = 1;
    Plegatomode = 0;
    setPvolume(96);
    Pkeyshift = 64;
    Prcvchn   = 0;
    setPpanning(64);
    Pvelsns   = 64;
    Pveloffs  = 64;
    Pkeylimit = 15;
    defaultsinstrument();
    ctl.defaults();
}

void Part::defaultsinstrument()
{
    ZERO(Pname, PART_MAX_NAME_LEN);

    info.Ptype = 0;
    ZERO(info.Pauthor, MAX_INFO_TEXT_SIZE + 1);
    ZERO(info.Pcomments, MAX_INFO_TEXT_SIZE + 1);

    Pkitmode  = 0;
    Pdrummode = 0;

    for(int n = 0; n < NUM_KIT_ITEMS; ++n) {
        kit[n].Penabled    = 0;
        kit[n].Pmuted      = 0;
        kit[n].Pminkey     = 0;
        kit[n].Pmaxkey     = 127;
        kit[n].Padenabled  = 0;
        kit[n].Psubenabled = 0;
        kit[n].Ppadenabled = 0;
        ZERO(kit[n].Pname, PART_MAX_NAME_LEN);
        kit[n].Psendtoparteffect = 0;
        if(n != 0)
            setkititemstatus(n, 0);
    }
    kit[0].Penabled   = 1;
    kit[0].Padenabled = 1;
    kit[0].adpars->defaults();
    kit[0].subpars->defaults();
    kit[0].padpars->defaults();

    for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx) {
        partefx[nefx]->defaults();
        Pefxroute[nefx] = 0; //route to next effect
    }
}



/*
 * Cleanup the part
 */
void Part::cleanup(bool final)
{
    for(int k = 0; k < POLIPHONY; ++k)
        KillNotePos(k);
    for(int i = 0; i < synth->buffersize; ++i) {
        partoutl[i] = final ? 0.0f : denormalkillbuf[i];
        partoutr[i] = final ? 0.0f : denormalkillbuf[i];
    }
    ctl.resetall();
    for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx)
        partefx[nefx]->cleanup();
    for(int n = 0; n < NUM_PART_EFX + 1; ++n)
        for(int i = 0; i < synth->buffersize; ++i) {
            partfxinputl[n][i] = final ? 0.0f : denormalkillbuf[i];
            partfxinputr[n][i] = final ? 0.0f : denormalkillbuf[i];
        }
}

template<class T>
static inline void nullify(T &t) {delete t; t = NULL; }

Part::~Part()
{
    cleanup(true);
    for(int n = 0; n < NUM_KIT_ITEMS; ++n) {
        nullify(kit[n].adpars);
        nullify(kit[n].subpars);
        nullify(kit[n].padpars);
        delete [] kit[n].Pname;
    }

    delete [] Pname;
    delete [] partoutl;
    delete [] partoutr;
    for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx)
        delete (partefx[nefx]);
    for(int n = 0; n < NUM_PART_EFX + 1; ++n) {
        delete [] partfxinputl[n];
        delete [] partfxinputr[n];
    }
}

/*
 * Note On Messages
 */
void Part::NoteOn(unsigned char note,
                  unsigned char velocity,
                  int masterkeyshift)
{
    // Legato and MonoMem used vars:
    int  posb = POLIPHONY - 1; // Just a dummy initial value.
    bool legatomodevalid = false; //true when legato mode is determined applicable.
    bool doinglegato     = false; // true when we determined we do a legato note.
    bool ismonofirstnote = false; /*(In Mono/Legato) true when we determined
                                     no other notes are held down or sustained.*/
    int lastnotecopy     = lastnote; //Useful after lastnote has been changed.

    if(!Pnoteon || (note < Pminkey) || (note > Pmaxkey))
        return;

    // MonoMem stuff:
    if(!Ppolymode) { // If Poly is off
        monomemnotes.push_back(note); // Add note to the list.
        monomem[note].velocity  = velocity; // Store this note's velocity.
        monomem[note].mkeyshift = masterkeyshift; /* Store masterkeyshift too,
                         I'm not sure why though... */
        if((partnote[lastpos].status != KEY_PLAYING)
           && (partnote[lastpos].status != KEY_RELASED_AND_SUSTAINED))
            ismonofirstnote = true;  // No other keys are held or sustained.
    } else if(not monomemnotes.empty()) // Poly mode is On so just make sure the list is empty.
        monomemnotes.clear();

    lastnote = note;

    int pos = -1;
    for(int i = 0; i < POLIPHONY; ++i) {
        if(partnote[i].status == KEY_OFF) {
            pos = i;
            break;
        }
    }

    if(Plegatomode && Pdrummode) {
        if(Ppolymode) {
            fprintf(stderr,
                "ZynAddSubFX WARNING: Poly and Legato modes are both On, that should not happen ! ... Disabling Legato mode ! - (Part.cpp::NoteOn(..))\n");
            Plegatomode = false;
        } else {
            // Legato mode is on and applicable.
            legatomodevalid = true;
            if((not ismonofirstnote) && (lastlegatomodevalid)) {
                // At least one other key is held or sustained, and the
                // previous note was played while in valid legato mode.
                doinglegato = true; // So we'll do a legato note.
                pos  = lastpos; // A legato note uses same pos as previous..
                posb = lastposb; // .. same goes for posb.
            } else {
                // Legato mode is valid, but this is only a first note.
                for(int i = 0; i < POLIPHONY; ++i)
                    if((partnote[i].status == KEY_PLAYING)
                       || (partnote[i].status == KEY_RELASED_AND_SUSTAINED))
                        RelaseNotePos(i);

                // Set posb
                posb = (pos + 1) % POLIPHONY; //We really want it (if the following fails)
                for(int i = 0; i < POLIPHONY; ++i)
                    if((partnote[i].status == KEY_OFF) && (pos != i)) {
                        posb = i;
                        break;
                    }
            }
            lastposb = posb; // Keep a trace of used posb
        }
    } else     // Legato mode is either off or non-applicable.
    if(!Ppolymode) {   //if the mode is 'mono' turn off all other notes
        for(int i = 0; i < POLIPHONY; ++i)
            if(partnote[i].status == KEY_PLAYING)
                RelaseNotePos(i);
        RelaseSustainedKeys();
    }
    lastlegatomodevalid = legatomodevalid;

    if(pos == -1) { //No appliciable note, exit with some level of grace
        fprintf(stderr, "NOTES TOO MANY (> POLIPHONY) - (Part.cpp::NoteOn(..))\n");
        setkeylimit(Pkeylimit);
        return;
    }

    //start the note
    partnote[pos].status = KEY_PLAYING;
    partnote[pos].note   = note;
    if(legatomodevalid) {
        partnote[posb].status = KEY_PLAYING;
        partnote[posb].note   = note;
    }

    //this computes the velocity sensing of the part
    float vel = VelF(velocity / 127.0f, Pvelsns);

    //compute the velocity offset
    vel += (Pveloffs - 64.0f) / 64.0f;
    if(vel < 0.0f)
        vel = 0.0f;
    else
    if(vel > 1.0f)
        vel = 1.0f;

    //compute the keyshift
    int partkeyshift = (int)Pkeyshift - 64;
    int keyshift     = masterkeyshift + partkeyshift;

    //initialise note frequency
    float notebasefreq;
    if(Pdrummode == 0) {
        notebasefreq = microtonal->getnotefreq(note, keyshift);
        if(notebasefreq < 0.0f)
            return;                  //the key is no mapped
    }
    else
        notebasefreq = 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);

    //Portamento
    if(oldfreq < 1.0f)
        oldfreq = notebasefreq;           //this is only the first note is played

    // For Mono/Legato: Force Portamento Off on first
    // notes. That means it is required that the previous note is
    // still held down or sustained for the Portamento to activate
    // (that's like Legato).
    int portamento = 0;
    if((Ppolymode != 0) || (not ismonofirstnote))
        // I added a third argument to the
        // ctl.initportamento(...) function to be able
        // to tell it if we're doing a legato note.
        portamento = ctl.initportamento(oldfreq, notebasefreq, doinglegato);

    if(portamento)
        ctl.portamento.noteusing = pos;
    oldfreq = notebasefreq;

    lastpos = pos; // Keep a trace of used pos.

    if(doinglegato) {
        // Do Legato note
        if(!Pkitmode) { // "normal mode" legato note
            if(kit[0].Padenabled
               && partnote[pos].kititem[0].adnote
               && partnote[posb].kititem[0].adnote) {
                partnote[pos].kititem[0].adnote->legatonote(
                        notebasefreq, vel, portamento, note, true); //'true' is to tell it it's being called from here.
                partnote[posb].kititem[0].adnote->legatonote(
                        notebasefreq, vel, portamento, note, true);
            }

            if((kit[0].Psubenabled)
               && partnote[pos].kititem[0].subnote
               && partnote[posb].kititem[0].subnote) {
                partnote[pos].kititem[0].subnote->legatonote(
                    notebasefreq, vel, portamento, note, true);
                partnote[posb].kititem[0].subnote->legatonote(
                    notebasefreq, vel, portamento, note, true);
            }

            if(kit[0].Ppadenabled
               && partnote[pos].kititem[0].padnote
               && partnote[posb].kititem[0].padnote) {
                partnote[pos].kititem[0].padnote->legatonote(
                    notebasefreq, vel, portamento, note, true);
                partnote[posb].kititem[0].padnote->legatonote(
                    notebasefreq, vel, portamento, note, true);
            }
        }
        else {   // "kit mode" legato note
            int ci = 0;
            for(int item = 0; item < NUM_KIT_ITEMS; ++item) {
                if(kit[item].Pmuted)
                    continue;
                if((note < kit[item].Pminkey) || (note > kit[item].Pmaxkey))
                    continue;

                if((lastnotecopy < kit[item].Pminkey)
                   || (lastnotecopy > kit[item].Pmaxkey))
                    continue;  // We will not perform legato across 2 key regions.

                partnote[pos].kititem[ci].sendtoparteffect =
                    (kit[item].Psendtoparteffect <
                     NUM_PART_EFX ? kit[item].Psendtoparteffect :
                     NUM_PART_EFX);                                        //if this parameter is 127 for "unprocessed"
                partnote[posb].kititem[ci].sendtoparteffect =
                    (kit[item].Psendtoparteffect <
                     NUM_PART_EFX ? kit[item].Psendtoparteffect :
                     NUM_PART_EFX);

                auto &kitem1 = partnote[pos].kititem[ci];
                auto &kitem2 = partnote[pos].kititem[ci];
                if(kit[item].Padenabled && kit[item].adpars && kitem1.adnote && kitem2.adnote) {
                    kitem1.adnote->legatonote(notebasefreq, vel, portamento, note, true);
                    kitem2.adnote->legatonote(notebasefreq, vel, portamento, note, true);
                }
                if(kit[item].Psubenabled && kit[item].subpars && kitem1.subnote && kitem2.subnote) {
                    kitem1.subnote->legatonote(notebasefreq, vel, portamento, note, true);
                    kitem2.subnote->legatonote(notebasefreq, vel, portamento, note, true);
                }
                if(kit[item].Ppadenabled && kit[item].padpars && kitem1.padnote && kitem2.padnote) {
                    kitem1.padnote->legatonote(notebasefreq, vel, portamento, note, true);
                    kitem2.padnote->legatonote(notebasefreq, vel, portamento, note, true);
                }

                if(kit[item].adpars || kit[item].subpars || kit[item].padpars) {
                    ci++;
                    if(Pkitmode == 2) //Single mode
                        break;
                }
            }
            if(ci == 0) {
                // No legato were performed at all, so pretend nothing happened:
                monomemnotes.pop_back(); // Remove last note from the list.
                lastnote = lastnotecopy; // Set lastnote back to previous value.
            }
        }
        return; // Ok, Legato note done, return.
    }

    partnote[pos].itemsplaying = 0;
    if(legatomodevalid)
        partnote[posb].itemsplaying = 0;

    if(Pkitmode == 0) { //init the notes for the "normal mode"
        auto &kitem  = partnote[pos].kititem[0];
        SynthPars pars{ctl, notebasefreq, vel, portamento, note, false};
        kitem.sendtoparteffect = 0;
        if(kit[0].Padenabled)
            kitem.adnote = new ADnote(kit[0].adpars, pars);

        if(kit[0].Psubenabled)
            kitem.subnote = new SUBnote(kit[0].subpars, pars);

        if(kit[0].Ppadenabled)
            kitem.padnote = new PADnote(kit[0].padpars, pars);

        if(kit[0].Padenabled || kit[0].Psubenabled || kit[0].Ppadenabled)
            partnote[pos].itemsplaying++;

        // Spawn another note (but silent) if legatomodevalid==true
        if(legatomodevalid) {
            SynthPars lpars{ctl, notebasefreq, vel, portamento, note, true};
            auto &lkitem  = partnote[posb].kititem[0];
            partnote[posb].kititem[0].sendtoparteffect = 0;
            if(kit[0].Padenabled)
                lkitem.adnote = new ADnote(kit[0].adpars, lpars);
            if(kit[0].Psubenabled)
                lkitem.subnote = new SUBnote(kit[0].subpars, lpars);
            if(kit[0].Ppadenabled)
                lkitem.padnote = new PADnote(kit[0].padpars, lpars);
            if(kit[0].Padenabled || kit[0].Psubenabled || kit[0].Ppadenabled)
                partnote[posb].itemsplaying++;
        }
    }
    else    //init the notes for the "kit mode"
        for(int item = 0; item < NUM_KIT_ITEMS; ++item) {
            if(kit[item].Pmuted != 0)
                continue;
            if((note < kit[item].Pminkey) || (note > kit[item].Pmaxkey))
                continue;

            int ci = partnote[pos].itemsplaying; //ci=current item

            //if this parameter is 127 for "unprocessed"
            partnote[pos].kititem[ci].sendtoparteffect =
                (kit[item].Psendtoparteffect < NUM_PART_EFX ?
                 kit[item].Psendtoparteffect : NUM_PART_EFX);

            if((kit[item].adpars != NULL) && ((kit[item].Padenabled) != 0))
                partnote[pos].kititem[ci].adnote = new ADnote(
                    kit[item].adpars,
                    &ctl,
                    notebasefreq,
                    vel,
                    portamento,
                    note,
                    false);

            if((kit[item].subpars != NULL) && ((kit[item].Psubenabled) != 0))
                partnote[pos].kititem[ci].subnote = new SUBnote(
                    kit[item].subpars,
                    &ctl,
                    notebasefreq,
                    vel,
                    portamento,
                    note,
                    false);

            if((kit[item].padpars != NULL) && ((kit[item].Ppadenabled) != 0))
                partnote[pos].kititem[ci].padnote = new PADnote(
                    kit[item].padpars,
                    &ctl,
                    notebasefreq,
                    vel,
                    portamento,
                    note,
                    false);

            // Spawn another note (but silent) if legatomodevalid==true
            if(legatomodevalid) {
                partnote[posb].kititem[ci].sendtoparteffect =
                    (kit[item].Psendtoparteffect <
                     NUM_PART_EFX ? kit[item].Psendtoparteffect :
                     NUM_PART_EFX);                                                                                                                 //if this parameter is 127 for "unprocessed"

                if((kit[item].adpars != NULL)
                   && ((kit[item].Padenabled) != 0))
                    partnote[posb].kititem[ci].adnote = new ADnote(
                        kit[item].adpars,
                        &ctl,
                        notebasefreq,
                        vel,
                        portamento,
                        note,
                        false);

                // Spawn another note (but silent) if legatomodevalid==true
                if(legatomodevalid) {

                    //if this parameter is 127 for "unprocessed"
                    partnote[posb].kititem[ci].sendtoparteffect =
                        (kit[item].Psendtoparteffect <
                         NUM_PART_EFX ? kit[item].Psendtoparteffect :
                         NUM_PART_EFX);

                    if((kit[item].adpars != NULL)
                       && ((kit[item].Padenabled) != 0))
                        partnote[posb].kititem[ci].adnote = new ADnote(
                            kit[item].adpars,
                            &ctl,
                            notebasefreq,
                            vel,
                            portamento,
                            note,
                            true);//true for silent.
                    if((kit[item].subpars != NULL)
                       && ((kit[item].Psubenabled) != 0))
                        partnote[posb].kititem[ci].subnote =
                            new SUBnote(kit[item].subpars,
                                        &ctl,
                                        notebasefreq,
                                        vel,
                                        portamento,
                                        note,
                                        true);
                    if((kit[item].padpars != NULL)
                       && ((kit[item].Ppadenabled) != 0))
                        partnote[posb].kititem[ci].padnote =
                            new PADnote(kit[item].padpars,
                                        &ctl,
                                        notebasefreq,
                                        vel,
                                        portamento,
                                        note,
                                        true);

                    if((kit[item].adpars != NULL) || (kit[item].subpars != NULL))
                        partnote[posb].itemsplaying++;
                }

            if((kit[item].adpars != NULL) || (kit[item].subpars != NULL)) {
                partnote[pos].itemsplaying++;
                if(((kit[item].Padenabled != 0)
                    || (kit[item].Psubenabled != 0)
                    || (kit[item].Ppadenabled != 0))
                   && (Pkitmode == 2))
                    break;
            }
        }


    //this only relase the keys if there is maximum number of keys allowed
    setkeylimit(Pkeylimit);
}

/*
 * Note Off Messages
 */
void Part::NoteOff(unsigned char note) //relase the key
{
    // This note is released, so we remove it from the list.
    if(!monomemnotes.empty())
        monomemnotes.remove(note);

    for(int i = POLIPHONY - 1; i >= 0; i--) //first note in, is first out if there are same note multiple times
        if((partnote[i].status == KEY_PLAYING) && (partnote[i].note == note)) {
            if(ctl.sustain.sustain == 0) { //the sustain pedal is not pushed
                if((Ppolymode == 0) && (not monomemnotes.empty()))
                    MonoMemRenote();  // To play most recent still held note.
                else
                    RelaseNotePos(i);
                /// break;
            }
            else    //the sustain pedal is pushed
                partnote[i].status = KEY_RELASED_AND_SUSTAINED;
        }
}

void Part::PolyphonicAftertouch(unsigned char note,
                                unsigned char velocity,
                                int masterkeyshift)
{
    (void) masterkeyshift;
    if(!Pnoteon || (note < Pminkey) || (note > Pmaxkey))
        return;
    if(Pdrummode)
        return;

    // MonoMem stuff:
    if(!Ppolymode)   // if Poly is off

        monomem[note].velocity = velocity;       // Store this note's velocity.


    for(int i = 0; i < POLIPHONY; ++i)
        if((partnote[i].note == note) && (partnote[i].status == KEY_PLAYING)) {
            /* update velocity */
            // compute the velocity offset
            float vel =
                VelF(velocity / 127.0f, Pvelsns) + (Pveloffs - 64.0f) / 64.0f;
            vel = (vel < 0.0f) ? 0.0f : vel;
            vel = (vel > 1.0f) ? 1.0f : vel;

            if(!Pkitmode) { // "normal mode"
                if(kit[0].Padenabled)
                    partnote[i].kititem[0].adnote->setVelocity(vel);
                if(kit[0].Psubenabled)
                    partnote[i].kititem[0].subnote->setVelocity(vel);
                if(kit[0].Ppadenabled)
                    partnote[i].kititem[0].padnote->setVelocity(vel);
            }
            else     // "kit mode"
                for(int item = 0; item < NUM_KIT_ITEMS; ++item) {
                    if(kit[item].Pmuted)
                        continue;
                    if((note < kit[item].Pminkey)
                       || (note > kit[item].Pmaxkey))
                        continue;

                    if(kit[item].Padenabled)
                        partnote[i].kititem[item].adnote->setVelocity(vel);
                    if(kit[item].Psubenabled)
                        partnote[i].kititem[item].subnote->setVelocity(vel);
                    if(kit[item].Ppadenabled)
                        partnote[i].kititem[item].padnote->setVelocity(vel);
                }
        }

}

/*
 * Controllers
 */
void Part::SetController(unsigned int type, int par)
{
    switch(type) {
        case C_pitchwheel:
            ctl.setpitchwheel(par);
            break;
        case C_expression:
            ctl.setexpression(par);
            setPvolume(Pvolume); //update the volume
            break;
        case C_portamento:
            ctl.setportamento(par);
            break;
        case C_panning:
            ctl.setpanning(par);
            setPpanning(Ppanning); //update the panning
            break;
        case C_filtercutoff:
            ctl.setfiltercutoff(par);
            break;
        case C_filterq:
            ctl.setfilterq(par);
            break;
        case C_bandwidth:
            ctl.setbandwidth(par);
            break;
        case C_modwheel:
            ctl.setmodwheel(par);
            break;
        case C_fmamp:
            ctl.setfmamp(par);
            break;
        case C_volume:
            ctl.setvolume(par);
            if(ctl.volume.receive != 0)
                volume = ctl.volume.volume;
            else
                setPvolume(Pvolume);
            break;
        case C_sustain:
            ctl.setsustain(par);
            if(ctl.sustain.sustain == 0)
                RelaseSustainedKeys();
            break;
        case C_allsoundsoff:
            AllNotesOff(); //Panic
            break;
        case C_resetallcontrollers:
            ctl.resetall();
            RelaseSustainedKeys();
            if(ctl.volume.receive != 0)
                volume = ctl.volume.volume;
            else
                setPvolume(Pvolume);
            setPvolume(Pvolume); //update the volume
            setPpanning(Ppanning); //update the panning

            for(int item = 0; item < NUM_KIT_ITEMS; ++item) {
                if(kit[item].adpars == NULL)
                    continue;
                kit[item].adpars->GlobalPar.Reson->
                sendcontroller(C_resonance_center, 1.0f);

                kit[item].adpars->GlobalPar.Reson->
                sendcontroller(C_resonance_bandwidth, 1.0f);
            }
            //more update to add here if I add controllers
            break;
        case C_allnotesoff:
            RelaseAllKeys();
            break;
        case C_resonance_center:
            ctl.setresonancecenter(par);
            for(int item = 0; item < NUM_KIT_ITEMS; ++item) {
                if(kit[item].adpars == NULL)
                    continue;
                kit[item].adpars->GlobalPar.Reson->
                sendcontroller(C_resonance_center,
                               ctl.resonancecenter.relcenter);
            }
            break;
        case C_resonance_bandwidth:
            ctl.setresonancebw(par);
            kit[0].adpars->GlobalPar.Reson->
            sendcontroller(C_resonance_bandwidth, ctl.resonancebandwidth.relbw);
            break;
    }
}
/*
 * Relase the sustained keys
 */

void Part::RelaseSustainedKeys()
{
    // Let's call MonoMemRenote() on some conditions:
    if((Ppolymode == 0) && (not monomemnotes.empty()))
        if(monomemnotes.back() != lastnote) // Sustain controller manipulation would cause repeated same note respawn without this check.
            MonoMemRenote();  // To play most recent still held note.

    for(int i = 0; i < POLIPHONY; ++i)
        if(partnote[i].status == KEY_RELASED_AND_SUSTAINED)
            RelaseNotePos(i);
}

/*
 * Relase all keys
 */

void Part::RelaseAllKeys()
{
    for(int i = 0; i < POLIPHONY; ++i)
        if((partnote[i].status != KEY_RELASED)
           && (partnote[i].status != KEY_OFF)) //thanks to Frank Neumann
            RelaseNotePos(i);
}

// Call NoteOn(...) with the most recent still held key as new note
// (Made for Mono/Legato).
void Part::MonoMemRenote()
{
    unsigned char mmrtempnote = monomemnotes.back(); // Last list element.
    monomemnotes.pop_back(); // We remove it, will be added again in NoteOn(...).
    if(Pnoteon == 0)
        RelaseNotePos(lastpos);
    else
        NoteOn(mmrtempnote, monomem[mmrtempnote].velocity,
               monomem[mmrtempnote].mkeyshift);
}

/*
 * Release note at position
 */
void Part::RelaseNotePos(int pos)
{
    for(int j = 0; j < NUM_KIT_ITEMS; ++j) {
        if(partnote[pos].kititem[j].adnote)
            partnote[pos].kititem[j].adnote->relasekey();

        if(partnote[pos].kititem[j].subnote)
            partnote[pos].kititem[j].subnote->relasekey();

        if(partnote[pos].kititem[j].padnote)
            partnote[pos].kititem[j].padnote->relasekey();
    }
    partnote[pos].status = KEY_RELASED;
}


/*
 * Kill note at position
 */
void Part::KillNotePos(int pos)
{
    partnote[pos].status = KEY_OFF;
    partnote[pos].note   = -1;
    partnote[pos].time   = 0;
    partnote[pos].itemsplaying = 0;

    for(int j = 0; j < NUM_KIT_ITEMS; ++j) {
        nullify(partnote[pos].kititem[j].adnote);
        nullify(partnote[pos].kititem[j].subnote);
        nullify(partnote[pos].kititem[j].padnote);
    }
    if(pos == ctl.portamento.noteusing) {
        ctl.portamento.noteusing = -1;
        ctl.portamento.used      = 0;
    }
}


/*
 * Set Part's key limit
 */
void Part::setkeylimit(unsigned char Pkeylimit)
{
    this->Pkeylimit = Pkeylimit;
    int keylimit = Pkeylimit;
    if(keylimit == 0)
        keylimit = POLIPHONY - 5;

    //release old keys if the number of notes>keylimit
    if(Ppolymode != 0) {
        int notecount = 0;
        for(int i = 0; i < POLIPHONY; ++i)
            if((partnote[i].status == KEY_PLAYING)
               || (partnote[i].status == KEY_RELASED_AND_SUSTAINED))
                notecount++;

        int oldestnotepos = -1;
        if(notecount > keylimit)   //find out the oldest note
            for(int i = 0; i < POLIPHONY; ++i) {
                int maxtime = 0;
                if(((partnote[i].status == KEY_PLAYING)
                    || (partnote[i].status == KEY_RELASED_AND_SUSTAINED))
                   && (partnote[i].time > maxtime)) {
                    maxtime = partnote[i].time;
                    oldestnotepos = i;
                }
            }
        if(oldestnotepos != -1)
            RelaseNotePos(oldestnotepos);
    }
}


/*
 * Prepare all notes to be turned off
 */
void Part::AllNotesOff()
{
    killallnotes = 1;
}

void Part::RunNote(unsigned int k)
{
    unsigned noteplay = 0;
    for(int item = 0; item < partnote[k].itemsplaying; ++item) {
        int sendcurrenttofx = partnote[k].kititem[item].sendtoparteffect;

        for(unsigned type = 0; type < 3; ++type) {
            //Select a note
            SynthNote **note = NULL;
            if(type == 0)
                note = &partnote[k].kititem[item].adnote;
            else if(type == 1)
                note = &partnote[k].kititem[item].subnote;
            else if(type == 2)
                note = &partnote[k].kititem[item].padnote;

            //Process if it exists
            if(!(*note))
                continue;
            noteplay++;

            float *tmpoutr = getTmpBuffer();
            float *tmpoutl = getTmpBuffer();
            (*note)->noteout(&tmpoutl[0], &tmpoutr[0]);

            if((*note)->finished()) {
                delete (*note);
                (*note) = NULL;
            }
            for(int i = 0; i < synth->buffersize; ++i) { //add the note to part(mix)
                partfxinputl[sendcurrenttofx][i] += tmpoutl[i];
                partfxinputr[sendcurrenttofx][i] += tmpoutr[i];
            }
            returnTmpBuffer(tmpoutr);
            returnTmpBuffer(tmpoutl);
        }
    }

    //Kill note if there is no synth on that note
    if(noteplay == 0)
        KillNotePos(k);
}

/*
 * Compute Part samples and store them in the partoutl[] and partoutr[]
 */
void Part::ComputePartSmps()
{
    for(unsigned nefx = 0; nefx < NUM_PART_EFX + 1; ++nefx)
        for(int i = 0; i < synth->buffersize; ++i) {
            partfxinputl[nefx][i] = 0.0f;
            partfxinputr[nefx][i] = 0.0f;
        }

    for(unsigned k = 0; k < POLIPHONY; ++k) {
        if(partnote[k].status == KEY_OFF)
            continue;
        partnote[k].time++;
        //get the sampledata of the note and kill it if it's finished
        RunNote(k);
    }


    //Apply part's effects and mix them
    for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx) {
        if(!Pefxbypass[nefx]) {
            partefx[nefx]->out(partfxinputl[nefx], partfxinputr[nefx]);
            if(Pefxroute[nefx] == 2)
                for(int i = 0; i < synth->buffersize; ++i) {
                    partfxinputl[nefx + 1][i] += partefx[nefx]->efxoutl[i];
                    partfxinputr[nefx + 1][i] += partefx[nefx]->efxoutr[i];
                }
        }
        int routeto = ((Pefxroute[nefx] == 0) ? nefx + 1 : NUM_PART_EFX);
        for(int i = 0; i < synth->buffersize; ++i) {
            partfxinputl[routeto][i] += partfxinputl[nefx][i];
            partfxinputr[routeto][i] += partfxinputr[nefx][i];
        }
    }
    for(int i = 0; i < synth->buffersize; ++i) {
        partoutl[i] = partfxinputl[NUM_PART_EFX][i];
        partoutr[i] = partfxinputr[NUM_PART_EFX][i];
    }

    //Kill All Notes if killallnotes!=0
    if(killallnotes != 0) {
        for(int i = 0; i < synth->buffersize; ++i) {
            float tmp = (synth->buffersize_f - i) / synth->buffersize_f;
            partoutl[i] *= tmp;
            partoutr[i] *= tmp;
        }
        for(int k = 0; k < POLIPHONY; ++k)
            KillNotePos(k);
        killallnotes = 0;
        for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx)
            partefx[nefx]->cleanup();
    }
    ctl.updateportamento();
}

/*
 * Parameter control
 */
void Part::setPvolume(char Pvolume_)
{
    Pvolume = Pvolume_;
    volume  =
        dB2rap((Pvolume - 96.0f) / 96.0f * 40.0f) * ctl.expression.relvolume;
}

void Part::setPpanning(char Ppanning_)
{
    Ppanning = Ppanning_;
    panning  = Ppanning / 127.0f + ctl.panning.pan;
    if(panning < 0.0f)
        panning = 0.0f;
    else
    if(panning > 1.0f)
        panning = 1.0f;
}

template<class T>
static inline void nullify(T &t) {delete t; t = NULL; }

/*
 * Enable or disable a kit item
 */
void Part::setkititemstatus(unsigned kititem, bool Penabled_)
{
    //nonexistent kit item and the first kit item is always enabled
    if((kititem == 0) || (kititem >= NUM_KIT_ITEMS))
        return;

    Kit &kkit = kit[kititem];

    //no need to update if
    if(kkit.Penabled == Penabled_)
        return;
    kkit.Penabled = Penabled_;

    if(Penabled_ == 0) {
        //nullify(kkit.adpars);
        //nullify(kkit.subpars);
        //nullify(kkit.padpars);
        kkit.Pname[0] = '\0';

        //Reset notes s.t. stale buffers will not get read
        for(int k = 0; k < POLIPHONY; ++k)
            KillNotePos(k);
    }
    else {
        //All parameters must be NULL in this case
        //assert(!(kkit.adpars || kkit.subpars || kkit.padpars));
        //kkit.adpars  = new ADnoteParameters(fft);
        //kkit.subpars = new SUBnoteParameters();
        //kkit.padpars = new PADnoteParameters(fft);
    }
}

void Part::add2XMLinstrument(XMLwrapper *xml)
{
    xml->beginbranch("INFO");
    xml->addparstr("name", (char *)Pname);
    xml->addparstr("author", (char *)info.Pauthor);
    xml->addparstr("comments", (char *)info.Pcomments);
    xml->addpar("type", info.Ptype);
    xml->endbranch();


    xml->beginbranch("INSTRUMENT_KIT");
    xml->addpar("kit_mode", Pkitmode);
    xml->addparbool("drum_mode", Pdrummode);

    for(int i = 0; i < NUM_KIT_ITEMS; ++i) {
        xml->beginbranch("INSTRUMENT_KIT_ITEM", i);
        xml->addparbool("enabled", kit[i].Penabled);
        if(kit[i].Penabled != 0) {
            xml->addparstr("name", (char *)kit[i].Pname);

            xml->addparbool("muted", kit[i].Pmuted);
            xml->addpar("min_key", kit[i].Pminkey);
            xml->addpar("max_key", kit[i].Pmaxkey);

            xml->addpar("send_to_instrument_effect", kit[i].Psendtoparteffect);

            xml->addparbool("add_enabled", kit[i].Padenabled);
            if((kit[i].Padenabled != 0) && (kit[i].adpars != NULL)) {
                xml->beginbranch("ADD_SYNTH_PARAMETERS");
                kit[i].adpars->add2XML(xml);
                xml->endbranch();
            }

            xml->addparbool("sub_enabled", kit[i].Psubenabled);
            if((kit[i].Psubenabled != 0) && (kit[i].subpars != NULL)) {
                xml->beginbranch("SUB_SYNTH_PARAMETERS");
                kit[i].subpars->add2XML(xml);
                xml->endbranch();
            }

            xml->addparbool("pad_enabled", kit[i].Ppadenabled);
            if((kit[i].Ppadenabled != 0) && (kit[i].padpars != NULL)) {
                xml->beginbranch("PAD_SYNTH_PARAMETERS");
                kit[i].padpars->add2XML(xml);
                xml->endbranch();
            }
        }
        xml->endbranch();
    }
    xml->endbranch();

    xml->beginbranch("INSTRUMENT_EFFECTS");
    for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx) {
        xml->beginbranch("INSTRUMENT_EFFECT", nefx);
        xml->beginbranch("EFFECT");
        partefx[nefx]->add2XML(xml);
        xml->endbranch();

        xml->addpar("route", Pefxroute[nefx]);
        partefx[nefx]->setdryonly(Pefxroute[nefx] == 2);
        xml->addparbool("bypass", Pefxbypass[nefx]);
        xml->endbranch();
    }
    xml->endbranch();
}

void Part::add2XML(XMLwrapper *xml)
{
    //parameters
    xml->addparbool("enabled", Penabled);
    if((Penabled == 0) && (xml->minimal))
        return;

    xml->addpar("volume", Pvolume);
    xml->addpar("panning", Ppanning);

    xml->addpar("min_key", Pminkey);
    xml->addpar("max_key", Pmaxkey);
    xml->addpar("key_shift", Pkeyshift);
    xml->addpar("rcv_chn", Prcvchn);

    xml->addpar("velocity_sensing", Pvelsns);
    xml->addpar("velocity_offset", Pveloffs);

    xml->addparbool("note_on", Pnoteon);
    xml->addparbool("poly_mode", Ppolymode);
    xml->addpar("legato_mode", Plegatomode);
    xml->addpar("key_limit", Pkeylimit);

    xml->beginbranch("INSTRUMENT");
    add2XMLinstrument(xml);
    xml->endbranch();

    xml->beginbranch("CONTROLLER");
    ctl.add2XML(xml);
    xml->endbranch();
}

int Part::saveXML(const char *filename)
{
    XMLwrapper *xml;
    xml = new XMLwrapper();

    xml->beginbranch("INSTRUMENT");
    add2XMLinstrument(xml);
    xml->endbranch();

    int result = xml->saveXMLfile(filename);
    delete (xml);
    return result;
}

int Part::loadXMLinstrument(const char *filename)
{
    XMLwrapper *xml = new XMLwrapper();
    if(xml->loadXMLfile(filename) < 0) {
        delete (xml);
        return -1;
    }

    if(xml->enterbranch("INSTRUMENT") == 0)
        return -10;
    getfromXMLinstrument(xml);
    xml->exitbranch();

    delete (xml);
    return 0;
}

void Part::applyparameters(void)
{
    for(int n = 0; n < NUM_KIT_ITEMS; ++n)
        if(kit[n].Ppadenabled && kit[n].padpars)
            kit[n].padpars->applyparameters();
}

void Part::getfromXMLinstrument(XMLwrapper *xml)
{
    if(xml->enterbranch("INFO")) {
        xml->getparstr("name", (char *)Pname, PART_MAX_NAME_LEN);
        xml->getparstr("author", (char *)info.Pauthor, MAX_INFO_TEXT_SIZE);
        xml->getparstr("comments", (char *)info.Pcomments, MAX_INFO_TEXT_SIZE);
        info.Ptype = xml->getpar("type", info.Ptype, 0, 16);

        xml->exitbranch();
    }

    if(xml->enterbranch("INSTRUMENT_KIT")) {
        Pkitmode  = xml->getpar127("kit_mode", Pkitmode);
        Pdrummode = xml->getparbool("drum_mode", Pdrummode);

        setkititemstatus(0, 0);
        for(int i = 0; i < NUM_KIT_ITEMS; ++i) {
            if(xml->enterbranch("INSTRUMENT_KIT_ITEM", i) == 0)
                continue;
            setkititemstatus(i, xml->getparbool("enabled", kit[i].Penabled));
            if(kit[i].Penabled == 0) {
                xml->exitbranch();
                continue;
            }

            xml->getparstr("name", (char *)kit[i].Pname, PART_MAX_NAME_LEN);

            kit[i].Pmuted  = xml->getparbool("muted", kit[i].Pmuted);
            kit[i].Pminkey = xml->getpar127("min_key", kit[i].Pminkey);
            kit[i].Pmaxkey = xml->getpar127("max_key", kit[i].Pmaxkey);

            kit[i].Psendtoparteffect = xml->getpar127(
                "send_to_instrument_effect",
                kit[i].Psendtoparteffect);

            kit[i].Padenabled = xml->getparbool("add_enabled",
                                                kit[i].Padenabled);
            if(xml->enterbranch("ADD_SYNTH_PARAMETERS")) {
                kit[i].adpars->getfromXML(xml);
                xml->exitbranch();
            }

            kit[i].Psubenabled = xml->getparbool("sub_enabled",
                                                 kit[i].Psubenabled);
            if(xml->enterbranch("SUB_SYNTH_PARAMETERS")) {
                kit[i].subpars->getfromXML(xml);
                xml->exitbranch();
            }

            kit[i].Ppadenabled = xml->getparbool("pad_enabled",
                                                 kit[i].Ppadenabled);
            if(xml->enterbranch("PAD_SYNTH_PARAMETERS")) {
                kit[i].padpars->getfromXML(xml);
                xml->exitbranch();
            }

            xml->exitbranch();
        }

        xml->exitbranch();
    }


    if(xml->enterbranch("INSTRUMENT_EFFECTS")) {
        for(int nefx = 0; nefx < NUM_PART_EFX; ++nefx) {
            if(xml->enterbranch("INSTRUMENT_EFFECT", nefx) == 0)
                continue;
            if(xml->enterbranch("EFFECT")) {
                partefx[nefx]->getfromXML(xml);
                xml->exitbranch();
            }

            Pefxroute[nefx] = xml->getpar("route",
                                          Pefxroute[nefx],
                                          0,
                                          NUM_PART_EFX);
            partefx[nefx]->setdryonly(Pefxroute[nefx] == 2);
            Pefxbypass[nefx] = xml->getparbool("bypass", Pefxbypass[nefx]);
            xml->exitbranch();
        }
        xml->exitbranch();
    }
}

void Part::getfromXML(XMLwrapper *xml)
{
    Penabled = xml->getparbool("enabled", Penabled);

    setPvolume(xml->getpar127("volume", Pvolume));
    setPpanning(xml->getpar127("panning", Ppanning));

    Pminkey   = xml->getpar127("min_key", Pminkey);
    Pmaxkey   = xml->getpar127("max_key", Pmaxkey);
    Pkeyshift = xml->getpar127("key_shift", Pkeyshift);
    Prcvchn   = xml->getpar127("rcv_chn", Prcvchn);

    Pvelsns  = xml->getpar127("velocity_sensing", Pvelsns);
    Pveloffs = xml->getpar127("velocity_offset", Pveloffs);

    Pnoteon     = xml->getparbool("note_on", Pnoteon);
    Ppolymode   = xml->getparbool("poly_mode", Ppolymode);
    Plegatomode = xml->getparbool("legato_mode", Plegatomode); //older versions
    if(!Plegatomode)
        Plegatomode = xml->getpar127("legato_mode", Plegatomode);
    Pkeylimit = xml->getpar127("key_limit", Pkeylimit);


    if(xml->enterbranch("INSTRUMENT")) {
        getfromXMLinstrument(xml);
        xml->exitbranch();
    }

    if(xml->enterbranch("CONTROLLER")) {
        ctl.getfromXML(xml);
        xml->exitbranch();
    }
}
