import asyncio
from playwright.async_api import async_playwright
async def main():
 state={'ready':True,'pairing':False,'pairing_mode':False,'rebooting':False,'message':'Ready','presets':[],'bulbs':[{'id':1,'name':'First'},{'id':2,'name':'Second'}]}
 commands=[];errors=[]
 async with async_playwright() as p:
  browser=await p.chromium.launch(executable_path='/home/user/.cache/ms-playwright/chromium-1208/chrome-linux64/chrome',args=['--no-sandbox'])
  page=await browser.new_page(viewport={'width':390,'height':844})
  page.on('pageerror',lambda e:errors.append(str(e)))
  async def api(route):
   path=route.request.url.split('/api/')[1]
   if path=='status':await route.fulfill(json=state);return
   d=route.request.post_data_json
   if path=='presets':
    if d['action']=='save':
     if 'id' in d:
      index=next(i for i,preset in enumerate(state['presets']) if preset['id']==d['id'])
      state['presets'][index]=d
     else:state['presets'].append(dict(d,id=len(state['presets'])+1))
    else:state['presets']=[p for p in state['presets'] if p['id']!=d['id']]
    await route.fulfill(json={'presets':state['presets']});return
   commands.append(d)
   if path=='settings' and 'flicker' in d:
    bulb=next(b for b in state['bulbs'] if b['id']==d['id'])
    bulb.update(flicker=d['flicker'],flicker_speed=d['flicker_speed'],flicker_range=d['flicker_range'],flicker_brightness=d['brightness'],flicker_kelvin=d['kelvin'])
   if path=='control':next(b for b in state['bulbs'] if b['id']==d['id'])['flicker']=False
   await route.fulfill(status=202,body='OK')
  await page.route('**/api/**',api)
  await page.goto('http://127.0.0.1:8765')
  await page.wait_for_function('cards.size===2')
  card=page.locator('.card').first;other=page.locator('.card').nth(1)
  await page.locator('#presets .preset-save').click()
  await page.locator('#preset-name').fill('Warm evening')
  await page.locator('#preset-targets input[value="2"]').uncheck()
  await page.locator('#save-preset').click()
  await page.locator('#preset-dialog').wait_for(state='hidden')
  assert state['presets'][0]['lights'][0]['mode']=='white'
  await page.locator('#presets .preset-select').select_option('1')
  await page.locator('#presets .preset-apply').click()
  await page.wait_for_function('!working&&!jobs.size')
  assert {'id':1,'action':'temperature','value':3000} in commands
  await card.locator('.color-tab').click()
  await card.locator('.picker').fill('#80c0ff')
  await page.wait_for_function('!working&&!jobs.size')
  await card.locator('.brightness').fill('35')
  await card.locator('.brightness').dispatch_event('change')
  await page.wait_for_function('!working&&!jobs.size')
  await page.locator('#presets .preset-save').click()
  await page.locator('#preset-name').fill('Soft blue')
  await page.locator('#preset-targets input[value="2"]').check()
  await page.locator('#save-preset').click()
  await page.locator('#preset-dialog').wait_for(state='hidden')
  await page.reload()
  await page.wait_for_function('savedPresets.length===2')
  other=page.locator('.card').nth(1)
  await page.locator('#presets .preset-select').select_option('2')
  commands.clear()
  await page.locator('#presets .preset-apply').click()
  await page.wait_for_function('!working&&!jobs.size')
  assert {'id':1,'action':'color','hue':210,'saturation':50} in commands
  assert {'id':1,'action':'brightness','value':35} in commands
  assert await page.locator('.card').first.locator('.brightness').input_value()=='35'
  assert {'id':2,'action':'temperature','value':3000} in commands
  assert {'id':2,'action':'brightness','value':70} in commands
  assert len(state['presets'][0]['lights'])==1 and len(state['presets'][1]['lights'])==2
  await other.locator('.flicker-speed').fill('90')
  await other.locator('.flicker-speed').dispatch_event('change')
  await other.locator('.flicker-range').fill('7')
  await other.locator('.flicker-range').dispatch_event('change')
  await other.locator('.flicker').click()
  await page.wait_for_function('cards.get(2).flicker')
  await page.locator('#presets .preset-save').click()
  await page.locator('#preset-name').fill('Quiet evening')
  await page.locator('#save-preset').click()
  await page.locator('#preset-dialog').wait_for(state='hidden')
  effect=state['presets'][2]['lights'][1]
  assert effect['flicker'] and effect['flicker_speed']==90 and effect['flicker_range']==7
  await page.reload()
  await page.wait_for_function('savedPresets.length===3')
  await page.locator('#presets .preset-select').select_option('3')
  commands.clear()
  await page.locator('#presets .preset-apply').click()
  await page.wait_for_function('!working&&!jobs.size')
  assert commands[-1]['action']=='effect' and commands[-1]['flicker']
  assert commands[-1]['flicker_speed']==90 and commands[-1]['flicker_range']==7
  assert not state['bulbs'][0]['flicker'] and state['bulbs'][1]['flicker']
  other=page.locator('.card').nth(1)
  await other.locator('.flicker-speed').fill('120')
  await other.locator('.flicker-speed').dispatch_event('change')
  await page.wait_for_function('!working&&!jobs.size')
  assert state['bulbs'][1]['flicker_speed']==120
  await page.locator('#presets .preset-update').click()
  assert await page.locator('#preset-name').input_value()=='Quiet evening'
  assert await page.locator('#preset-targets input:checked').count()==2
  await page.locator('#save-preset').click()
  await page.locator('#preset-dialog').wait_for(state='hidden')
  assert len(state['presets'])==3
  assert state['presets'][2]['id']==3 and state['presets'][2]['lights'][1]['flicker_speed']==120
  assert await page.evaluate('document.documentElement.scrollWidth<=innerWidth')
  assert await page.locator('.card .saved-presets').count()==0
  assert await page.locator('#presets').count()==1
  await page.locator('#theme-toggle').click()
  assert await page.evaluate("document.documentElement.dataset.theme")=='dark'
  await page.screenshot(path='tests/theme-dark-mobile.png',full_page=True)
  await page.reload()
  await page.wait_for_function('online')
  assert await page.evaluate("document.documentElement.dataset.theme")=='dark'
  await page.set_viewport_size({'width':1160,'height':1000})
  await page.screenshot(path='tests/theme-dark-desktop.png',full_page=True)
  await page.locator('#theme-toggle').click()
  assert await page.evaluate("document.documentElement.dataset.theme")=='light'
  await page.screenshot(path='tests/theme-light-desktop.png',full_page=True)
  await page.locator('#presets .preset-select').select_option('2')
  page.on('dialog',lambda dialog:dialog.accept())
  await page.locator('#presets .preset-delete').click()
  await page.wait_for_function('savedPresets.length===2')
  assert not errors,errors
  await browser.close()
 print('PASS single/group presets, flicker on/off and speed/range saved/restored, live slider update, update-in-place without duplication, reload, dark mode and mobile layout.')
asyncio.run(main())
