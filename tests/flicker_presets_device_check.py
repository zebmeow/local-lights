import asyncio,json,urllib.request,urllib.error
from pathlib import Path
from playwright.async_api import async_playwright
BASE='http://192.168.1.192'
def get():return json.load(urllib.request.urlopen(BASE+'/api/status',timeout=4))
def post(path,data):
 req=urllib.request.Request(BASE+path,data=json.dumps(data).encode(),headers={'Content-Type':'application/json','X-Local-Control':'1'})
 return urllib.request.urlopen(req,timeout=5).read()
async def ready():
 for n in range(25):
  try:
   s=get()
   if s['ready'] and not s['rebooting']:return s
  except Exception:pass
  await asyncio.sleep(1)
 raise RuntimeError('Controller unavailable')
async def main():
 s=await ready()
 original=json.loads(Path('tests/before-flicker-presets.json').read_text())
 for old in original['presets']:
  migrated=next(p for p in s['presets'] if p['id']==old['id'])
  assert migrated['name']==old['name']
  for oldlight in old['lights']:
   newlight=next(p for p in migrated['lights'] if p['id']==oldlight['id'])
   assert all(newlight[k]==v for k,v in oldlight.items())
   assert not newlight['flicker'] and newlight['flicker_speed']==45 and newlight['flicker_range']==3
 print('PASS existing preset migration',flush=True)
 lights=[dict(id=1,color='#ffb66b',mode='white',brightness=27,kelvin=2200,flicker=True,flicker_speed=90,flicker_range=2),
         dict(id=2,color='#ffb66b',mode='white',brightness=17,kelvin=2200,flicker=False,flicker_speed=60,flicker_range=4)]
 data={'action':'save','name':'Temporary flicker check','lights':lights}
 preset=next(p for p in json.loads(post('/api/presets',data))['presets'] if p['name']==data['name'])
 try:
  for change in [{'flicker_speed':0},{'flicker_speed':121},{'flicker_range':0},{'flicker_range':21},{'flicker':'yes'}]:
   invalid=dict(data,lights=[dict(lights[0],**change)])
   try:post('/api/presets',invalid);raise AssertionError('Invalid effect accepted')
   except urllib.error.HTTPError as e:assert e.code==400
  post('/api/mode',{'pairing':False})
  await asyncio.sleep(8)
  s=await ready()
  assert preset in s['presets']
  before=s['confirmed']
  async with async_playwright() as p:
   browser=await p.chromium.launch(executable_path='/home/user/.cache/ms-playwright/chromium-1208/chrome-linux64/chrome',args=['--no-sandbox'])
   page=await browser.new_page()
   await page.goto(BASE)
   await page.wait_for_function('online&&cards.size===2')
   await page.locator('#presets .preset-select').select_option(str(preset['id']))
   await page.locator('#presets .preset-apply').click()
   await page.wait_for_function('!working&&!jobs.size',timeout=30000)
   assert await page.locator('.card').first.locator('.flicker-speed').input_value()=='90'
   assert await page.locator('.card').first.locator('.flicker-range').input_value()=='2'
   await browser.close()
  for n in range(20):
   s=get()
   if s['confirmed']>=before+6 and s['in_flight']==0:break
   await asyncio.sleep(1)
  else:raise AssertionError(s)
  a,b=s['bulbs']
  assert a['flicker'] and a['flicker_speed']==90 and a['flicker_range']==2
  assert not b['flicker'] and b['flicker_speed']==60 and b['flicker_range']==4
  assert s['failed']==0
  print('PASS speed/range and mixed on/off group persisted after reboot, restored from UI, and acknowledged by both bulbs with browser closed.',flush=True)
 finally:
  current=get()
  first=current['bulbs'][0]
  if first['flicker'] and first['flicker_speed']==90 and first['flicker_range']==2:
   post('/api/settings',dict(lights[0],flicker=False))
  if next((p for p in current['presets'] if p['id']==preset['id']),None)==preset:
   post('/api/presets',{'action':'delete','id':preset['id']})
 print('Temporary preset cleaned up; existing presets preserved.',flush=True)
 print(json.dumps(get()),flush=True)
asyncio.run(main())
