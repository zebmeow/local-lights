import asyncio,json,urllib.request,urllib.error
from playwright.async_api import async_playwright
BASE='http://192.168.1.192'
def get():return json.load(urllib.request.urlopen(BASE+'/api/status',timeout=4))
def post(data):
 req=urllib.request.Request(BASE+'/api/presets',data=json.dumps(data).encode(),headers={'Content-Type':'application/json','X-Local-Control':'1'})
 return json.load(urllib.request.urlopen(req,timeout=5))
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
 entry={'id':1,'color':'#ffb66b','mode':'white','kelvin':2200,'brightness':27,'flicker':False,'flicker_speed':45,'flicker_range':3}
 data={'action':'save','name':'Temporary update check','lights':[entry]}
 response=post(data);preset=next(p for p in response['presets'] if p['name']==data['name'])
 try:
  async with async_playwright() as p:
   browser=await p.chromium.launch(executable_path='/home/user/.cache/ms-playwright/chromium-1208/chrome-linux64/chrome',args=['--no-sandbox'])
   page=await browser.new_page()
   errors=[];page.on('pageerror',lambda e:errors.append(str(e)))
   await page.goto(BASE)
   await page.wait_for_function('online&&cards.size===2')
   await page.locator('#presets .preset-select').select_option(str(preset['id']))
   card=page.locator('.card').first
   await card.locator('.flicker-speed').fill('100')
   await card.locator('.flicker-speed').dispatch_event('change')
   await card.locator('.flicker-range').fill('4')
   await card.locator('.flicker-range').dispatch_event('change')
   await page.locator('#presets .preset-update').click()
   assert await page.locator('#preset-name').input_value()==data['name']
   assert await page.locator('#preset-targets input:checked').count()==1
   await page.locator('#save-preset').click()
   await page.locator('#preset-dialog').wait_for(state='hidden')
   updated=next(p for p in get()['presets'] if p['id']==preset['id'])
   assert updated['lights'][0]['flicker_speed']==100 and updated['lights'][0]['flicker_range']==4
   assert sum(p['name']==data['name'] for p in get()['presets'])==1
   assert not errors,errors
   await browser.close()
  # Verify update storage accepts enabled flicker too, without changing actual lights.
  updated['lights'][0]['flicker']=True
  post(dict(updated,action='save'))
  if s['presets']:
   try:post(dict(updated,action='save',name=s['presets'][0]['name']));raise AssertionError('Duplicate accepted')
   except urllib.error.HTTPError as e:assert e.code==400
  req=urllib.request.Request(BASE+'/api/mode',data=b'{"pairing":false}',headers={'Content-Type':'application/json','X-Local-Control':'1'})
  urllib.request.urlopen(req,timeout=5).read()
  await asyncio.sleep(8)
  after=await ready()
  assert next(p for p in after['presets'] if p['id']==preset['id'])==updated
  print('PASS update button, preserved preset ID, no duplicate, speed/range and enabled flicker survive reboot, duplicate-name protection.',flush=True)
 finally:
  current=next((p for p in get()['presets'] if p['id']==preset['id']),None)
  if current and current['name']==data['name']:post({'action':'delete','id':preset['id']})
 print('Temporary test preset removed; user presets preserved.',flush=True)
asyncio.run(main())
