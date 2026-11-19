import asyncio,json,time,urllib.request,urllib.error
from pathlib import Path
from playwright.async_api import async_playwright
ROOT=Path(__file__).resolve().parents[1];BASE='http://192.168.1.192'
def get():return json.load(urllib.request.urlopen(BASE+'/api/status',timeout=5))
def post(path,data):
 req=urllib.request.Request(BASE+path,data=json.dumps(data).encode(),headers={'Content-Type':'application/json','X-Local-Control':'1'})
 return urllib.request.urlopen(req,timeout=5).read()
async def main():
 for i in range(30):
  try:
   state=get()
   if 'flicker' in state['bulbs'][0]:break
  except Exception:pass
  await asyncio.sleep(1)
 else:raise RuntimeError('New firmware unavailable')
 original=state['bulbs'][0]['name'];errors=[]
 async with async_playwright() as p:
  browser=await p.chromium.launch(executable_path='/home/user/.cache/ms-playwright/chromium-1208/chrome-linux64/chrome',headless=True,args=['--no-sandbox'])
  page=await browser.new_page(viewport={'width':390,'height':844})
  page.on('pageerror',lambda e:errors.append(str(e)))
  await page.goto(BASE)
  await page.wait_for_function('online&&cards.size===2')
  card=page.locator('.card').first
  await card.locator('.rename').click()
  await page.locator('#rename-name').fill(original+' test')
  await page.locator('#save-name').click()
  await page.locator('#rename-dialog').wait_for(state='hidden')
  assert get()['bulbs'][0]['name']==original+' test'
  await card.locator('.flicker').click()
  await page.wait_for_function('cards.get(1).flicker===true')
  await page.screenshot(path=str(ROOT/'tests/flicker-mobile.png'),full_page=True)
  assert await page.evaluate('document.documentElement.scrollWidth<=innerWidth')
  assert not errors,errors
  await browser.close()
 post('/api/settings',{'id':2,'flicker':True,'brightness':65,'kelvin':2700})
 baseline=get();heaps=[]
 for n in range(35):
  await asyncio.sleep(2)
  state=get();heaps.append(state['free_heap'])
  assert all(b['flicker'] for b in state['bulbs'])
  assert state['in_flight']<=2
  assert state['failed']==baseline['failed'],state
 print('Flicker without browser: commands acknowledged',state['confirmed']-baseline['confirmed'],'heap range',min(heaps),max(heaps),flush=True)
 assert state['confirmed']-baseline['confirmed']>=4
 for bulb in state['bulbs']:
  for n in range(20):
   try:post('/api/control',{'id':bulb['id'],'action':'temperature','value':3000});break
   except urllib.error.HTTPError as e:
    if e.code!=503:raise
    await asyncio.sleep(.3)
  else:raise RuntimeError('Manual override blocked')
 await asyncio.sleep(3)
 state=get()
 assert not any(b['flicker'] for b in state['bulbs'])
 before=state['confirmed']
 await asyncio.sleep(4)
 assert get()['confirmed']==before
 for invalid in [{'id':1,'name':''},{'id':1,'name':'x'*40},{'id':1,'flicker':True,'brightness':0},{'id':999,'name':'bad'}]:
  try:post('/api/settings',invalid);raise AssertionError('Invalid input accepted')
  except urllib.error.HTTPError as e:assert e.code==400
 post('/api/mode',{'pairing':False})
 await asyncio.sleep(8)
 for n in range(20):
  try:
   state=get()
   if not state['rebooting']:break
  except Exception:pass
  await asyncio.sleep(1)
 assert state['bulbs'][0]['name']==original+' test'
 assert len(state['bulbs'])==2 and not any(b['flicker'] for b in state['bulbs'])
 post('/api/settings',{'id':1,'name':original})
 state=get()
 (ROOT/'tests/flicker-device-result.json').write_text(json.dumps({'status':state,'effect_heap_min':min(heaps),'effect_heap_max':max(heaps),'test':'PASS'},indent=2))
 print('PASS rename UI, saved name after reboot, both effects without browser, bounded commands, manual override, invalid inputs, paired bulbs preserved. Original name restored.',flush=True)
asyncio.run(main())
