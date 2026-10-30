* 2N7002 (CJ C8545 / onsemi class): Vth 1.0..2.5 V at 250 uA, 25 C. Charge-based (EKV-style) channel
* with subthreshold conduction (the round-2 level-1 model has none) and a Vth tempco; body tied to
* source (DMOS), so the source-drain body diode is explicit. Ispec 520 uA puts 250 uA at Vgs = Vth;
* n 1.5 (89 mV/decade at 25 C); Rds ~1.6 ohm at Vgs 4.5 V (Vth 2.1).
* vth25: threshold at 25 C; tk: tempco V/K (onsemi 2N7002 fig. "Vth vs T": ~ -4 mV/K); tc: deg C.
.subckt E7002 d g s vth25=1.0 tk=-4m tc=25
.param vt={8.617e-5*(tc+273.15)}
.param vth={vth25+tk*(tc-25)}
.func lf(x) {ln(1+exp(x))*ln(1+exp(x))}
B1 d s I = 520u*( lf((V(g,s)-vth)/1.5/(2*vt)) - lf(((V(g,s)-vth)/1.5 - V(d,s))/(2*vt)) )
Cgs g s 25p
Cgd g d 5p
D1 s d DB7002
.model DB7002 D(IS=1e-12 N=1.1 RS=0.5 CJO=20p)
.ends
