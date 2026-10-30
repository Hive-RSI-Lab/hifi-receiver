* what a sub plays: two buffered 80 Hz RC poles on SUB_L (a sub's crossover, 12 dB/oct)
E1 l1i 0 subl 0 1
R1f l1i l1 1k
C1f l1 0 1.99u
E2 l2i 0 l1 0 1
R2f l2i l2 1k
C2f l2 0 1.99u
* and a sub's infrasonic filter: two buffered 20 Hz CR high-pass poles after the 80 Hz low-pass (band = what it reproduces)
E3 h1i 0 l2 0 1
C3f h1i h1 7.96u
R3f h1 0 1k
E4 h2i 0 h1 0 1
C4f h2i h2 7.96u
R4f h2 0 1k
