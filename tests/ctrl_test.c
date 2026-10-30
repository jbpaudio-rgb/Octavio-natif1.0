#include <stdio.h>
#include <stdint.h>
#include "../src/ctrl.c"
int16_t PV[P_COUNT]; int gotos; void ui_goto(int p){gotos=p;} void ui_params_changed(void){}
int main(void){
  uint32_t t=1000; ctrl_init(t);
  ctrl_button(CB_ENV,1,t); ctrl_button(CB_ENV,0,t+50); printf("page ENV=%d got %d\n",PGN_ENV,ctrl_page());
  t+=100; ctrl_button(CB_ENV,1,t); ctrl_button(CB_ENV,0,t+50); printf("latched mask %x\n",(unsigned)ctrl_locked());
  ctrl_button(CB_SAVE,1,t+500); ctrl_button(CB_SAVE,0,t+520); printf("overlay %d\n",ctrl_overlay());
  char tx[16],el[40]; int p; ctrl_name(tx,&p,el); printf("name '%s' %s\n",tx,el);
  ctrl_button(CB_SAVE,1,t+600); ctrl_button(CB_SAVE,0,t+620);
  printf("bank %d cur %d overlay %d\n",bank_count(),bank_current(),ctrl_overlay());
  ctrl_side(ROLE_PRESETS,-2,t+700); printf("pi %d\n",bank_current());
  return 0;}
