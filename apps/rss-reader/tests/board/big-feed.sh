#!/bin/sh
# Board tests L and C of RSS Reader (tests/BOARD-TESTS.md): writes feed.xml into the current folder, a feed served by the
# board itself (python3 -m http.server 8765) whose articles link the large pictures of tools/make_img_fixtures.py
# (jpeg-16mp.jpg, png-4mp.png, png-4mp-rgba.png, png-6mp.png, jpeg-6mp.jpg in the same folder):
#   item 0     the limits: a 16 MP JPEG, two 4.2 MP PNGs, a 6 MP PNG (over the PNG limit)
#   items 1-12 six 6 MP photos each (different addresses: about 4.7 MB per saved article), to reach the storage cap
set -e
base=${1:-http://127.0.0.1:8765}
{
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo "<rss version=\"2.0\"><channel><title>Big pictures</title><link>$base/</link><description>RSS Reader board tests</description>"
    echo "<item><title>16 MP JPEG and 4 MP PNGs</title><guid>big-0</guid><description><![CDATA[<p>The limits of the decoders.</p>"
    echo "<img src=\"$base/jpeg-16mp.jpg\" alt=\"JPEG 4800 x 3300\"><img src=\"$base/png-4mp.png\" alt=\"PNG 2048 x 2048\">"
    echo "<img src=\"$base/png-4mp-rgba.png\" alt=\"PNG 2048 x 2048 with alpha\"><img src=\"$base/png-6mp.png\" alt=\"PNG 3000 x 2000, over the limit\">"
    echo "]]></description></item>"
    for n in 1 2 3 4 5 6 7 8 9 10 11 12; do
        echo "<item><title>Six 6 MP photos, set $n</title><guid>big-$n</guid><description><![CDATA[<p>About 4.7 MB once saved.</p>"
        for k in 1 2 3 4 5 6; do echo "<img src=\"$base/jpeg-6mp.jpg?$n-$k\" alt=\"Photo $k of set $n\">"; done
        echo "]]></description></item>"
    done
    echo '</channel></rss>'
} > feed.xml
echo "feed.xml: $(grep -c '<item>' feed.xml) articles; serve this folder with: python3 -m http.server 8765"
