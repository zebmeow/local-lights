import asyncio,json
from pathlib import Path
from playwright.async_api import async_playwright
ROOT=Path(__file__).resolve().parents[1]
async def main():
 state={'ready':True,'pairing':False,'pairing_mode':False,'rebooting':False,'message':'Ready','free_heap':90000,'confirmed':0,'failed':0,'bulbs':[{'id':1,'endpoint':1,'name':'Cync bulb 1'},{'id':2,'endpoint':1,'name':'bulb2'}]}
 requests=[]; errors=[]
 async with async_playwright() as p:
  browser=await p.chromium.launch(executable_path='/home/user/.cache/ms-playwright/chromium-1208/chrome-linux64/chrome',headless=True,args=['--no-sandbox'])
  page=await browser.new_page(viewport={'width':1160,'height':950},device_scale_factor=1)
  page.on('pageerror',lambda e: errors.append(str(e)))
  async def api(route):
   path=route.request.url.split('/api/')[1]
   if path=='status': await route.fulfill(json=state);return
   data=route.request.post_data_json;requests.append((path,data))
   if path=='mode':state['pairing_mode']=data['pairing']
   if path=='pair':
    state['bulbs'].append({'id':3,'endpoint':1,'name':data['name']});state['message']='Bulb paired. Ready to control.'
   if path=='control':
    state['confirmed']+=1;state['message']='Light updated'
    next(b for b in state['bulbs'] if b['id']==data['id'])['flicker']=False
   if path=='settings':
    bulb=next(b for b in state['bulbs'] if b['id']==data['id'])
    if 'name' in data:bulb['name']=data['name']
    if 'flicker' in data:bulb['flicker']=data['flicker']
   await route.fulfill(status=202,body='OK')
  await page.route('**/api/**',api)
  await page.goto('http://127.0.0.1:8765')
  await page.locator('.card').nth(1).wait_for()
  card=page.locator('.card').first
  await card.locator('.color-tab').click()
  await page.wait_for_function('!working&&!jobs.size')
  await card.locator('.picker').fill('#80c0ff')
  await page.wait_for_function('!working&&!jobs.size')
  assert any(d.get('action')=='color' and d.get('hue')==210 and d.get('saturation')==50 for _,d in requests)
  assert await card.locator('.hex').input_value()=='#80C0FF'
  await card.locator('.hex').fill('#7050a0');await card.locator('.hex').press('Tab')
  await page.wait_for_function('!working&&!jobs.size')
  assert any(d.get('action')=='brightness' and d.get('value')==63 for _,d in requests)
  await card.locator('.rename').click()
  await page.locator('#rename-name').fill('Reading corner')
  await page.locator('#save-name').click()
  await page.locator('#rename-dialog').wait_for(state='hidden')
  assert await card.locator('h2').inner_text()=='Reading corner'
  await card.locator('.flicker').click()
  await page.wait_for_function('cards.get(1).flicker===true')
  assert state['bulbs'][0]['flicker']
  await card.locator('.flicker').click()
  await page.wait_for_function('cards.get(1).flicker===false')
  assert not state['bulbs'][0]['flicker']
  await page.screenshot(path=str(ROOT/'tests/desktop.png'),full_page=True)
  await page.set_viewport_size({'width':390,'height':844})
  assert await page.evaluate('document.documentElement.scrollWidth<=innerWidth')
  await page.screenshot(path=str(ROOT/'tests/mobile.png'),full_page=True)
  await page.locator('#add').click();await page.locator('#start-setup').click()
  await page.locator('#pair-form').wait_for(state='visible',timeout=15000)
  await page.wait_for_function('online&&expectedMode===null',timeout=15000)
  await page.locator('#light-name').fill('Reading corner');await page.locator('#setup-code').fill('12345678901')
  await page.locator('#pair-submit').click()
  await page.wait_for_function('cards.size===3',timeout=15000)
  await page.locator('#finish-dialog').click()
  await page.wait_for_function('expectedMode===null&&!state.pairing_mode',timeout=15000)
  assert not await page.locator('#setup').is_visible()
  await page.locator('#all-off').click();await page.wait_for_function('!working&&!jobs.size')
  assert {d['id'] for _,d in requests if d.get('action')=='power' and d.get('value')==0}=={1,2,3}
  assert not errors,errors
  print('PASS: color HSV/value mapping, hex entry, mobile layout, setup/reconnect flow, new-bulb rendering, all-off queue; no JS errors.')
  await browser.close()
asyncio.run(main())
