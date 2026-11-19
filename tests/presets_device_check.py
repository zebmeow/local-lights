import asyncio,json,urllib.request,urllib.error
from playwright.async_api import async_playwright
BASE='http://192.168.1.192'
def get():return json.load(urllib.request.urlopen(BASE+'/api/status',timeout=4))
def post(path,data):
 r=urllib.request.Request(BASE+path,data=json.dumps(data).encode(),headers={'Content-Type':'application/json','X-Local-Control':'1'})
 return urllib.request.urlopen(r,timeout=5).read()
async def main():
 for n in range(20):
  try:
   s=get()
   if 'presets' in s:break
  except Exception:pass
  await asyncio.sleep(1)
 else:raise RuntimeError('Firmware unavailable')
 test_presets=[]
 for mode,color,kelvin,brightness in [('white','#ffb66b',2700,65),('color','#80c0ff',3000,35)]:
  light={'id':1,'mode':mode,'color':color,'kelvin':kelvin,'brightness':brightness}
  lights=[light]
  if mode=='color':lights.append({'id':2,'mode':'white','color':'#ffb66b','kelvin':3000,'brightness':65})
  d={'action':'save','name':'Test preset '+mode,'lights':lights}
  reply=json.loads(post('/api/presets',d));test_presets.append(next(p for p in reply['presets'] if p['name']==d['name']))
 try:
  for change in [{'brightness':0},{'kelvin':0},{'color':'invalid'},{'mode':'bad'}]:
   try:post('/api/presets',dict(d,lights=[dict(light,**change)]));raise AssertionError('Invalid preset accepted')
   except urllib.error.HTTPError as e:assert e.code==400
  post('/api/mode',{'pairing':False})
  await asyncio.sleep(8)
  for n in range(20):
   try:
    s=get()
    if not s['rebooting']:break
   except Exception:pass
   await asyncio.sleep(1)
  for p in test_presets:assert p in s['presets']
  async with async_playwright() as p:
   browser=await p.chromium.launch(executable_path='/home/user/.cache/ms-playwright/chromium-1208/chrome-linux64/chrome',args=['--no-sandbox'])
   page=await browser.new_page()
   await page.goto(BASE)
   await page.wait_for_function('online&&savedPresets.length>=2')
   card=page.locator('.card').first
   for preset in reversed(test_presets):
    before=get()
    await page.locator('#presets .preset-select').select_option(str(preset['id']))
    await page.locator('#presets .preset-apply').click()
    await page.wait_for_function('!working&&!jobs.size')
    for n in range(15):
     s=get()
     if s['confirmed']>=before['confirmed']+2*len(preset['lights']):break
     await asyncio.sleep(1)
    else:raise AssertionError('No bulb acknowledgement')
    assert s['failed']==before['failed']
   await browser.close()
  print('PASS single/group presets saved on ESP32, survived reboot, fresh-browser visibility, both bulbs acknowledged mixed group settings, invalid inputs rejected.',flush=True)
 finally:
  for preset in test_presets:
   current=next((p for p in get()['presets'] if p['id']==preset['id']),None)
   if current==preset:post('/api/presets',{'action':'delete','id':preset['id']})
 print('Temporary test presets removed; user presets preserved.',flush=True)
 print(json.dumps(get()),flush=True)
asyncio.run(main())
