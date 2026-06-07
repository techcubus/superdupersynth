

;sspva ==0801==
   10 poke53280,2:poke53281,11:poke646,12
   11 tb$=chr$(16)
  100 print"{clr}{swlc}SuperSynth patch file viewer."
  110 print"{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}{CBM-Y}"
  111 poke198,0
  200 input"What file do you want to see?{down}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}{left}";pn$
  210 poke201,4:poke202,4:print
  300 print"Do you want this dumped to the printer?"
  310 geta$:ifa$=""then310
  320 ifa$="y"then 400
  330 ifa$="n"then 700
  335 ifa$="_"thenprint"{up}{up}{up}{up}":goto200
  340 goto310
  400 gosub1280
  410 if st=66thenprint"File Not Found.{up}{up}{up}{up}":goto200
  500 open7,4,7
  600 gosub3050
  610 goto2000
  700 gosub1280
  710 if st=66thenprint"File Not Found.{up}{up}{up}{up}":goto200
  800 gosub1060
  900 goto2000
 1060 print"{clr}{rvon}"pn$"{rvof}"
 1061 print"Z ="tab(21)z:print"FL ="tab(21)fl
 1070 print"Voice 1 ="tab(21)w1:print"Voice 2 ="tab(21)w2
 1080 print"Attack ="tab(21)at:print"Decay ="tab(21)de
 1090 print"Sustain ="tab(21)su:print"Release ="tab(21)re
 1100 print"Resonance ="tab(21)po
 1110 print"Sync speed ="tab(21)xt
 1120 print"Vibrato shape= "tab(21)vi
 1130 print"Vibrato speed= "tab(21)vs:print"Pulse shape voice 1 ="db
 1140 print"Pulse shape voice 2 ="dc:print"pulse shape voice 3 ="dd
 1150 print"Filter ="tab(21)vo
 1160 print"Step limit ="tab(21)sl:return
 1280 open1,8,0,pn$:input#1,z
 1290 input#1,fl:input#1,w1:input#1,w2:input#1,at:input#1,de:input#1,su
 1300 input#1,re:input#1,po:input#1,xt:input#1,vi:input#1,vs:input#1,db
 1310 input#1,dc:input#1,dd:input#1,vo:input#1,sl:close1:return
 2000 printtab(3)"{rvon}Press Return to view another file, or";:print"{rvon}the Space Bar to end."
 2100 geta$:ifa$=""then2100
 2200 ifa$=chr$(13)thenprint"{clr}":goto200
 2300 ifa$=" "thenpoke828,0:sys828
 2400 goto 2100
 3050 print#7,pn$
 3060 print#7,"Z =";tb$;"21";z:print#7,"FL =";tb$;"21";fl
 3070 print#7,"Voice 1 =";tb$;"21";w1:print#7,"Voice 2 =";tb$;"21";w2
 3080 print#7,"Attack =";tb$;"21";at:print#7,"Decay =";tb$;"21";de
 3090 print#7,"Sustain =";tb$;"21";su:print#7,"Release =";tb$;"21";re
 3100 print#7,"Resonance =";tb$;"21";po
 3110 print#7,"Sync speed =";tb$;"21";xt
 3120 print#7,"Vibrato shape= ";tb$;"21";vi
 3130 print#7,"Vibrato speed= ";tb$;"21";vs:print#7,"Pulse shape voice 1 ="db
 3140 print#7,"Pulse shape voice 2 ="dc:print#7,"pulse shape voice 3 ="dd
 3150 print#7,"Filter =";tb$;"21";vo
 3160 print#7,"Step limit =";tb$;"21";sl:return

