#!/bin/sh
# Fetch the power review's vendor documents, each into its own directory under /tmp/ds3/power/.
# Untrusted data: read with pdftotext only.
set -u
UA="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/120 Safari/537.36"
get() { d=/tmp/ds3/power/$1; mkdir -p "$d"; [ -s "$d/doc.pdf" ] || curl -sSL -A "$UA" -m 90 -o "$d/doc.pdf" "$2"; printf '%s %s\n' "$1" "$(file -b "$d/doc.pdf" | cut -c1-30)"; }
get tps54302      https://www.ti.com/lit/ds/symlink/tps54302.pdf
get tps54302evm https://www.ti.com/lit/ug/slvuap9/slvuap9.pdf
get tpa3118       https://www.ti.com/lit/ds/symlink/tpa3118d2.pdf
get tpa3221       https://www.ti.com/lit/ds/symlink/tpa3221.pdf
get tpa3220       https://www.ti.com/lit/ds/symlink/tpa3220.pdf
get lp5907        https://www.ti.com/lit/ds/symlink/lp5907.pdf
get ina186        https://www.ti.com/lit/ds/symlink/ina186.pdf
get ch224_en      https://done.land/assets/files/ch224aq_datasheet.pdf
get ch224_wch     https://www.wch-ic.com/download/file?id=360
get slva936       https://www.ti.com/lit/an/slva936/slva936.pdf
get slvaeb6       https://www.ti.com/lit/an/slvaeb6/slvaeb6.pdf
get tps2595      https://www.ti.com/lit/ds/symlink/tps2595.pdf
get ao4485        https://www.aosmd.com/res/datasheets/AO4485.pdf
get smbj          https://www.littelfuse.com/assets/docs/littelfuse/tvs-diodes/smbj-series.pdf
get sy8303   "https://datasheet.lcsc.com/datasheet/pdf/e1a61b84c1934bfd84df3c719ade0020.pdf?productCode=C97054"
get c88733   "https://datasheet.lcsc.com/datasheet/pdf/1f06f8929b144e399833f605cc15b596.pdf?productCode=C88733"
get sy8089   https://wmsc.lcsc.com/wmsc/upload/file/pdf/v2/lcsc/2001171834_Silergy-Corp-SY8089A1AAC_C479074.pdf
get swpa6045 https://wmsc.lcsc.com/wmsc/upload/file/pdf/v2/lcsc/2310251551_Sunlord-SWPA6045S100MT_C79272.pdf
get swpa4020 https://wmsc.lcsc.com/wmsc/upload/file/pdf/v2/lcsc/2310251551_Sunlord-SWPA4020S2R2NT_C305171.pdf
