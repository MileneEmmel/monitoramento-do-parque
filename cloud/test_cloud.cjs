const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync(__dirname+'/public/index.html','utf8');
const script=fs.readFileSync(__dirname+'/public/cloud.js','utf8');
const ids=new Set([...html.matchAll(/\bid="([^"]+)"/g)].map(m=>m[1]));
for(const m of script.matchAll(/\$\('([^']+)'\)/g))assert.ok(ids.has(m[1]),'Missing '+m[1]);
assert.doesNotMatch(html,/data-page="camera"|id="camera"|open-local|local-url/);
assert.doesNotMatch(script,/service_role|sb_secret|localStorage/);
class Element{
  constructor(){this.children=[];this.listeners={};this.attributes={};this.style={};this.classList={toggle(){},remove(){},add(){}};this.parentElement={append(){}};}
  addEventListener(k,f){this.listeners[k]=f;}
  setAttribute(k,v){this.attributes[k]=v;}
  getAttribute(k){return this.attributes[k];}
  removeAttribute(k){delete this.attributes[k];}
  replaceChildren(...v){this.children=v;}
  append(...v){this.children.push(...v);}
}
const elements=new Map();const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id);};
const context=vm.createContext({document:{getElementById:get,querySelectorAll(){return[];},querySelector(){return null;},createElement(){return new Element();},addEventListener(){},body:new Element(),hidden:false},window:{PARK_CLOUD:{},addEventListener(){}},location:{hash:'#overview'},matchMedia(){return {matches:false};},sessionStorage:{getItem(){return null;}},setInterval(){},setTimeout(){},URL,Blob,Intl,Date,AbortSignal,console});
vm.runInContext(script,context);const run=code=>vm.runInContext(code,context);
assert.equal(run("number('18446744073709551615')"),'18.446.744.073.709.551.615');
assert.equal(run("escapeHTML('<img onerror=x>')"),'&lt;img onerror=x&gt;');
assert.match(run('today()'),/^\d{4}-\d{2}-\d{2}$/);
run('setDates(7)');assert.equal(run('(new Date(end)-new Date(start))/86400000'),6);
run('drawChart([{label:"10h",entries:"9007199254740993",exits:"2",samples:1}])');
assert.equal(get('chart').children.length,1);
assert.match(get('chart').children[0].attributes['aria-label'],/9\.007\.199\.254\.740\.993/);
assert.equal(get('chart-detail').hidden,true);
get('chart').children[0].listeners.focus();assert.equal(get('chart-detail').hidden,false);
run('report=null;render()');assert.equal(get('entries').textContent,'—');assert.equal(get('full-history').children.length,0);
run('setMenu(false)');assert.equal(get('toggle-menu').attributes['aria-label'],'Expandir menu');
console.log('Cloud: DOM, camera separation, exact integers, escaping, dates, chart details, empty states, sidebar: PASS');
