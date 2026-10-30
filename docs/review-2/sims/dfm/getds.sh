# getds.sh C123 name : fetch LCSC datasheet PDF into /tmp/ds/dfm/<name>/ds.pdf and render page images
c=$1; n=$2; mkdir -p /tmp/ds/dfm/$n; cd /tmp/ds/dfm/$n || exit 1
curl -sSL -m 60 -A "Mozilla/5.0" -o p.html "https://www.lcsc.com/product-detail/$c.html"
u=$(grep -o "https://datasheet.lcsc.com/datasheet/pdf/[^\"]*productCode=$c" p.html | head -1)
[ -z "$u" ] && u=$(grep -o 'https://[^"]*\.pdf' p.html | grep -v ISO9001 | head -1)
curl -sSL -m 90 -A "Mozilla/5.0" -o ds.pdf "$u"; echo "$c $n $(file -b ds.pdf | cut -c1-30) $(pdfinfo ds.pdf 2>/dev/null | grep Pages)"
pdftoppm -r 90 -png ds.pdf pg 2>/dev/null; pdftotext -layout ds.pdf ds.txt 2>/dev/null
