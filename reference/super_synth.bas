  100 poke53281,0:poke53280,0:print"{clr}":poke214,10
  110 printtab(9)"{down}{yel}welcome to super-synth"
  120 print:printtab(3)"while waiting for frequencies to be
  130 print:printtab(5)"calculated, turn up the volume.":k=256:ch=35
  140 m=1.005:ma=64:dimf1(ma),f2(ma),f3(ma),f4(ma),g1(ma),g2(ma),g3(ma),g4(ma)
  150 dimh1(ma),h2(ma),h3(ma),h4(ma)
  160 print:readt,n:n1=n*m:n2=n*2:n3=n*2*m:n4=int(n/2):n5=int(n/2*m)
  170 f1(t)=int(n/k):f2(t)=n-(f1(t)*k):f3(t)=int(n1/k):f4(t)=int(n1-(f3(t)*k))
  180 g1(t)=int(n2/k):g2(t)=n2-(g1(t)*k):g3(t)=int(n3/k):g4(t)=int(n3-(g3(t)*k))
  190 h1(t)=int(n4/k):h2(t)=n4-(h1(t)*k):h3(t)=int(n5/k):h4(t)=int(n5-(h3(t)*k))
  200 ift<>chthen160
  210 ifch<>50thenfort=1to10:print:next:printtab(16)"thanks!":ch=50:goto160
  220 nf=8:nk=64:kb=197:v=54272:v1=v+1:v2=v:v3=v+8:v4=v+7:rn=rnd(-ti)
  230 fl=0:db=8:dc=8:dd=8:vo=31:vs=17:vi=90:xt=1:po=240:z=2:w1=33:w2=33:at=8:de=8
  240 su=8:re=8:gosub490
  250 gosub480:fort=vtov+23:poket,0:next:pokev+5,ad:pokev+6,sr:pokev+12,ad
  260 pokev+13,sr:pokev+3,db:pokev+10,dc:pokev+17,dd:pokev+14,vi:pokev+18,vs
  270 pokev+23,po:pokev+24,vo
  280 t=peek(kb):ift=nkthen280
  290 ift<nfthen470
  300 onzgoto310,330,340,350,360,320
  310 pokev1,g1(t):pokev2,g2(t):pokev3,g3(t):pokev4,g4(t):goto370
  320 pokev1,h1(t):pokev2,h2(t):pokev3,h3(t):pokev4,h4(t):goto370
  330 gosub460:goto370
  340 gosub460:pokev+15,f1(t)/.7:goto370
  350 gosub460:pokev+15,f1(t)/2:goto370
  360 gosub460:pokev+4,w1:pokev+11,w2:fory=1to10:next:goto430
  370 pokev+4,w1:pokev+11,w2
  380 ifz=4thenforu=1toslstepxt:pokev+1,u:ifpeek(kb)=tthennext
  390 ifz=4thengosub440:goto430
  400 iffl=1thenpokev,peek(v+27):pokev+7,peek(v+27):goto420
  410 iffl=2thenforu=1toslstep10:pokev+22,u:ifpeek(kb)=tthennext:gosub440:goto430
  420 ifpeek(kb)=tthen380
  430 pokev+4,w1-1:pokev+11,w2-1:pokev+15,0:goto280
  440 ifpeek(kb)<>nkthen440
  450 return
  460 pokev1,f1(t):pokev2,f2(t):pokev3,f3(t):pokev4,f4(t):return
  470 ont+1goto280,1050,280,1260,230,690,1200,280
  480 ad=at*16+de:sr=su*16+re:return
  490 poke53280,0:poke53281,0:print"{clr}{yel}"
  500 printtab(12)"keyboard screen{down}{down}"
  510 printtab(13)"f1 - normal":printtab(13)"f3 - new sound
  520 printtab(13)"f5 - save sound
  530 printtab(13)"f7 - load sound{down}{down}{wht}"
  540 printtab(5)"{CBM-M}{rvon} {rght} {rght} {SHIFT--} {rght} {rght} {rght} {SHIFT--} {rght} {rght} B {rght} {rght} {rvof}c{rvon} "
  550 printtab(5)"{CBM-M}{rvon} {rvof}2{rvon} {rvof}3{rvon} {SHIFT--} {rvof}5{rvon} {rvof}6{rvon} {rvof}7{rvon} {SHIFT--} {rvof}9{rvon} {rvof}0{rvon} {SHIFT--} {rvof}-{rvon} {rvof}\{rvon} {rvof}h{rvon} "
  560 printtab(5)"{CBM-M}{rvon} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {rvof} {yel}{SHIFT-*}I{wht}"
  570 printtab(5)"{CBM-M}{rvon}q{SHIFT--}w{SHIFT--}e{SHIFT--}r{SHIFT--}t{SHIFT--}y{SHIFT--}u{SHIFT--}i{SHIFT--}o{SHIFT--}p{SHIFT--}@{SHIFT--}*{SHIFT--}^{SHIFT--}z{rvof}  {yel}B"
  580 printtab(35)"B":printtab(6)"UCCCCCCCCCCCCCCCCCCCCCCCCCCCCK"
  590 printtab(6)"B":printtab(6)"B {wht}{CBM-N}{rvon} B {rght} {rght} {SHIFT--} {rght} {rght} {rght} {SHIFT--} {rght} {rght} {rvof}{CBM-H}"
  600 printtab(6)"{yel}B{wht} {CBM-N}{rvon} B {rvof}d{rvon} {rvof}f{rvon} {SHIFT--} {rvof}h{rvon} {rvof}j{rvon} {rvof}k{rvon} {SHIFT--} {rvof}:{rvon} {rvof};{rvon} {rvof}{CBM-H}"
  610 printtab(6)"{yel}J{SHIFT-*}{wht}{CBM-M}{rvon} B B {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {SHIFT--} {rvof}{CBM-H}"
  620 printtab(8)"{CBM-N}{rvon}zBx{SHIFT--}c{SHIFT--}v{SHIFT--}b{SHIFT--}n{SHIFT--}m{SHIFT--},{SHIFT--}.{SHIFT--}/{SHIFT--} {rvof}{CBM-H}{down}"
  630 printtab(8)"{yel}return for values screen":return
  640 data 62,2145,9,2408,14,2703,17,2864,22,3215,25,3608,30,4050,33,4291
  650 data 38,4817,41,5407,46,5728,49,6430,54,7217,12,8101,23,8583,20,9634
  660 data 31,10814,28,11457,39,12860,36,14435,47,16203,44,17167,55,19269
  670 data 59,2273,8,2551,16,3034,19,3406,24,3823,32,4547,35,5103,43,6069,48,6812
  680 data 51,7647,18,9094,21,10207,29,12139,34,13625,37,15294,45,18188,50,20415
  690 z=int(6*rnd(1))+1:fl=int(3*rnd(1))+0
  700 sl=int(255*rnd(1))+1
  710 w1=int(7*rnd(1))+1:onw1goto720,730,740,750,760,770,780
  720 w1=17:goto790
  730 w1=33:goto790
  740 w1=65:goto790
  750 w1=129:goto790
  760 w1=21:goto790
  770 w1=23:goto790
  780 w1=85
  790 w2=int(8*rnd(1))+1:onw2goto800,810,820,830,840,850,860,870
  800 w2=1:goto880
  810 w2=17:goto880
  820 w2=33:goto880
  830 w2=65:goto880
  840 w2=129:goto880
  850 w2=21:goto880
  860 w2=23:goto880
  870 w2=85
  880 at=int(10*rnd(1))+1:de=int(15*rnd(1))+1:su=int(15*rnd(1))+1
  890 re=int(15*rnd(1))+1:so=int(4*rnd(1))+1:onsogoto895,900,910,920
  895 po=240:goto930
  900 po=241:goto930
  910 po=242:goto930
  920 po=243
  930 xt=int(40*rnd(1))+1
  940 vs=int(4*rnd(1))+1:onvsgoto950,960,970,980
  950 vs=17:goto990
  960 vs=33:goto990
  970 vs=65:goto990
  980 vs=129
  990 vi=int(200*rnd(1))+55
 1000 db=int(8*rnd(1))+1:dc=int(8*rnd(1))+1:dd=int(8*rnd(1))+1
 1010 vo=int(3*rnd(1))+1:onvogoto1020,1030,1040
 1020 vo=31:goto250
 1030 vo=45:goto250
 1040 vo=79:goto250
 1050 poke53280,6:poke53281,6:poke198,0:print"{clr}{wht}":printtab(13)"values screen{down}{down}"
 1060 print"z ="tab(21)z:print"fl ="tab(21)fl
 1070 print"voice 1 ="tab(21)w1:print"voice 2 ="tab(21)w2
 1080 print"attack ="tab(21)at:print"decay ="tab(21)de
 1090 print"sustain ="tab(21)su:print"release ="tab(21)re
 1100 print"resonance ="tab(21)po
 1110 print"sync speed ="tab(21)xt
 1120 print"vibrato speed ="tab(21)vi
 1130 print"vibrato shape ="tab(21)vs:print"pulse shape voice 1 ="db
 1140 print"pulse shape voice 2 ="dc:print"pulse shape voice 3 ="dd
 1150 print"filter ="tab(21)vo
 1160 print"step limit ="tab(21)sl
 1170 print"{down}{rvon}press return for keyboard screen
 1180 geta$:ifa$<>chr$(13)then1180
 1190 gosub490:goto280
 1200 s$="":poke198,0:print"{clr}":poke214,9:print:poke211,4
 1210 input"sound to save";s$:ifs$=""thengosub490:goto280
 1220 open1,8,1,s$:print#1,z
 1230 print#1,fl:print#1,w1:print#1,w2:print#1,at:print#1,de:print#1,su
 1240 print#1,re:print#1,po:print#1,xt:print#1,vi:print#1,vs:print#1,db
 1250 print#1,dc:print#1,dd:print#1,vo:print#1,sl:close1:gosub490:goto280
 1260 s$="":poke198,0:print"{clr}":poke214,9:print:poke211,4
 1270 input"sound to load";s$:ifs$=""thengosub490:goto280
 1280 open1,8,0,s$:input#1,z
 1290 input#1,fl:input#1,w1:input#1,w2:input#1,at:input#1,de:input#1,su
 1300 input#1,re:input#1,po:input#1,xt:input#1,vi:input#1,vs:input#1,db
 1310 input#1,dc:input#1,dd:input#1,vo:input#1,sl:close1:gosub490:goto250
