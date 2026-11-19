import asyncio,json
from pathlib import Path
from playwright.async_api import async_playwright
ROOT=Path(__file__).resolve().parents[1]
async def main():
 errors=[]
 async with async_playwright() as p:
  browser=await p.chromium.launch(executable_path='/home/user/.cache/ms-playwright/chromium-1208/chrome-linux64/chrome',headless=True,args=['--no-sandbox'])
  page=await browser.new_page(viewport={'width':1160,'height':950})
  page.on('pageerror',lambda e:errors.append(str(e)))
  await page.goto('http://192.168.1.192')
  await page.wait_for_function('online&&cards.size===2')
  before=await page.evaluate('state.confirmed')
  card=page.locator('.card').first
  await card.locator('.color-tab').click()
  await card.locator('.picker').fill('#80c0ff')
  await page.wait_for_function('!working&&!jobs.size')
  await page.wait_for_function(f'state.confirmed>={before+2}&&!state.in_flight',timeout=45000)
  await page.screenshot(path=str(ROOT/'tests/device-desktop.png'),full_page=True)
  print('PASS real color picker commands acknowledged',flush=True)
  await page.set_viewport_size({'width':390,'height':844})
  assert await page.evaluate('document.documentElement.scrollWidth<=innerWidth')
  await page.screenshot(path=str(ROOT/'tests/device-mobile.png'),full_page=True)
  await page.locator('#add').click()
  await page.locator('#start-setup').click()
  await page.wait_for_function('online&&expectedMode===null&&state.pairing_mode',timeout=60000)
  assert await page.locator('#pair-form').is_visible()
  print('PAIRING MODE',await page.evaluate('JSON.stringify(state)'),flush=True)
  await page.locator('#finish-dialog').click()
  await page.wait_for_function('online&&expectedMode===null&&!state.pairing_mode',timeout=60000)
  assert await page.evaluate('cards.size===2')
  assert not await page.locator('#setup').is_visible()
  print('NORMAL MODE',await page.evaluate('JSON.stringify(state)'),flush=True)
  await card.locator('.white-tab').click()
  await page.wait_for_function('!working&&!jobs.size')
  await page.wait_for_function('state.confirmed>=1&&!state.in_flight',timeout=30000)
  assert not errors,errors
  result=await page.evaluate('state')
  (ROOT/'tests/device-result.json').write_text(json.dumps(result,indent=2))
  print('PASS device page, color control, mobile layout, Bluetooth mode restart, saved bulbs, control after restart, no JS errors.',flush=True)
  await browser.close()
asyncio.run(main())
