#pragma once

#define IDI_APPICON                 101
#define IDD_CONFIG                  102

// Every label carries its own id rather than sharing IDC_STATIC. A screen
// reader takes a control's accessible name from the static text immediately
// before it in the dialog template's z-order, and distinct ids make that
// pairing explicit and greppable instead of accidental.
#define IDC_VOICE_LABEL             1000
#define IDC_VOICE                   1001
#define IDC_RATE_LABEL              1002
#define IDC_RATE                    1003
#define IDC_PITCH_LABEL             1004
#define IDC_PITCH                   1005
#define IDC_PITCH_SPIN              1006
#define IDC_VOLUME_LABEL            1007
#define IDC_VOLUME                  1008
#define IDC_VOLUME_SPIN             1009
#define IDC_WORDDELAY_LABEL         1010
#define IDC_WORDDELAY               1011
#define IDC_WORDDELAY_SPIN          1012
#define IDC_REPEAT_LABEL            1013
#define IDC_REPEAT                  1014
#define IDC_REPEAT_SPIN             1015
#define IDC_CLOCK_LABEL             1016
#define IDC_CLOCK                   1017
#define IDC_SAMPLERATE_LABEL        1018
#define IDC_SAMPLERATE              1019
#define IDC_MONOTONE                1020
#define IDC_COMPRESSED              1021

#define IDC_ADVANCED_GROUP          1030
#define IDC_FRAMERATE_LABEL         1031
#define IDC_FRAMERATE               1032
#define IDC_LOGLEVEL_LABEL          1033
#define IDC_LOGLEVEL                1034

#define IDC_SPEAK                   1040
#define IDC_RESET                   1041
#define IDC_OPENLOGS                1042
#define IDC_STATUS                  1043
