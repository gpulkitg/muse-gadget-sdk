# Copyright (c) Meta Platforms, Inc. and affiliates.
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
"""Exercise real speaking controller and renderer without ESP-IDF or hardware."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SpeakingAnimation(unittest.TestCase):
    def test_interruptions_pauses_silent_captions_and_long_responses(self):
        source = r'''
#include <assert.h>
#include <math.h>
#include "muse_speaking.h"
static float hands(const muse_speaking_t *s) { return s->lift[0]+s->lift[1]+s->extend[0]+s->extend[1]; }
int main(void) {
    /* Every possible cutoff settles promptly; it never finishes a queued pair. */
    for (int cut=1;cut<180;cut++) {
        muse_speaking_t s={0};
        for(int i=0;i<cut;i++) muse_speaking_update(&s,true,.04f,i*.04f,.6f);
        for(int i=0;i<15;i++) muse_speaking_update(&s,false,.04f,i*.04f,0);
        assert(!s.active && hands(&s)<.01f);
    }
    muse_speaking_t s={0};
    bool raise=false, steps=false, open=false, rest=false;
    unsigned previous=MUSE_SPEAK_REST, sequence[40]={0}, actions=0;
    for(int i=0;i<1500;i++) {
        muse_speaking_update(&s,true,.04f,i*.04f,.6f);
        raise |= s.action==MUSE_SPEAK_RAISE && hands(&s)>.8f;
        steps |= s.action==MUSE_SPEAK_STEPS && s.inward[s.side]>.7f;
        open |= s.action==MUSE_SPEAK_OPEN && s.extend[0]>.7f && s.extend[1]>.7f;
        if(s.action!=MUSE_SPEAK_REST && s.action!=previous && actions<40) sequence[actions++]=s.action;
        previous=s.action;
        rest |= i>50 && s.action==MUSE_SPEAK_REST && hands(&s)<.05f;
        for(int j=0;j<2;j++) {
            assert(s.lift[j]>=0 && s.lift[j]<=1); assert(s.extend[j]>=0 && s.extend[j]<=1);
            assert(s.inward[j]>=0 && s.inward[j]<=1); assert(fabsf(s.wrist[j])<=1);
        }
    }
    assert(raise && steps && open && rest && actions>=9);
    for(unsigned i=0;i<actions;i++) assert(sequence[i]==MUSE_SPEAK_RAISE+i%3);
    /* The raise family actually changes hands, unlike the inward step beats
       or the simultaneous bilateral opening. */
    s=(muse_speaking_t){0};
    int first=-1,second=-1;
    for(int i=0;i<55;i++) {
        muse_speaking_update(&s,true,.04f,i*.04f,.6f);
        if(s.action==MUSE_SPEAK_RAISE && s.time>.3f && s.time<.7f) first=s.lift[1]>s.lift[0];
        if(s.action==MUSE_SPEAK_RAISE && s.time>1.3f && s.time<1.6f) second=s.lift[1]>s.lift[0];
    }
    assert(first>=0 && second>=0 && first!=second);
    /* A real pause closes down motion; sustained silent caption pacing still
       gets general gestures, because this main branch has no TTS. */
    s=(muse_speaking_t){0};
    for(int i=0;i<25;i++) muse_speaking_update(&s,true,.04f,i*.04f,.7f);
    for(int i=25;i<55;i++) muse_speaking_update(&s,true,.04f,i*.04f,0);
    assert(hands(&s)<.01f && s.energy<.001f);
    unsigned resumed=0;
    for(int i=55;i<100;i++) {
        muse_speaking_update(&s,true,.04f,i*.04f,.7f);
        if(!resumed && s.action!=MUSE_SPEAK_REST) resumed=s.action;
    }
    assert(resumed==MUSE_SPEAK_STEPS);
    s=(muse_speaking_t){0};
    float peak=0;
    for(int i=0;i<100;i++) { muse_speaking_update(&s,true,.04f,i*.04f,0); if(hands(&s)>peak) peak=hands(&s); }
    assert(peak>.7f && s.energy==0);
    /* Brief replies must rotate their openers too. */
    s=(muse_speaking_t){0};
    for(unsigned reply=0;reply<6;reply++) {
        unsigned opener=0;
        for(int i=0;i<13;i++) {
            muse_speaking_update(&s,true,.04f,i*.04f,.6f);
            if(!opener && s.action!=MUSE_SPEAK_REST) opener=s.action;
        }
        assert(opener==MUSE_SPEAK_RAISE+reply%3);
        for(int i=0;i<15;i++) muse_speaking_update(&s,false,.04f,i*.04f,0);
    }
    /* Preview warm-up at an unchanged mode time must not consume the opener. */
    s=(muse_speaking_t){0};
    for(int i=0;i<40;i++) muse_speaking_update(&s,true,.04f,0,.5f);
    assert(s.action==MUSE_SPEAK_REST && hands(&s)==0);
    return 0;
}
'''
        self.compile_run(source)

    def test_real_renderer_stays_in_bounds_across_repeated_state_changes(self):
        source = r'''
#include "muse_pixel.h"
static uint16_t pixels[64*64];
int main(void) {
    muse_pixel_set_size(64);
    for(int i=0;i<2000;i++) {
        float mt=(i%150)*.04f;
        muse_pose_t p={.t=i*.04f+1,.mode_t=mt,.mode=(i%150)<100 ? MUSE_MODE_SPEAKING : MUSE_MODE_LISTENING,
                      .level=(i%20)<12 ? .8f : 0};
        muse_pixel_render(&p);
        muse_pixel_scale(pixels,64,0,63,0,63);
    }
    return 0;
}
'''
        self.compile_run(source, renderer=True)

    def compile_run(self, source, renderer=False):
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            (p/'test.c').write_text(source)
            cmd=['cc','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                 '-I',str(ROOT/'components/muse'),str(p/'test.c')]
            if renderer: cmd.append(str(ROOT/'avatar/muse_pixel.c'))
            subprocess.run(cmd+['-lm','-o',str(p/'test')],check=True,capture_output=True,text=True)
            subprocess.run([str(p/'test')],check=True,capture_output=True,text=True)


if __name__ == '__main__':
    unittest.main()
