from pathlib import Path
root = Path(__file__).resolve().parent
html = (root/'web/index.html').read_text()
assert ')LIGHTPAGE"' not in html
(root/'firmware/main/web_page.h').write_text('#pragma once\nstatic const char web_page[] = R"LIGHTPAGE('+html+')LIGHTPAGE";\n')
