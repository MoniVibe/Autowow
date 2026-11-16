'use strict';
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const {Economy}=require('./economy.js');
const {config,simulate}=require('./scenario.js');
let count=0;
function test(name,fn){fn();count++;console.log(`PASS ${name}`);}
const fresh=()=>new Economy(config);
const stable=e=>JSON.stringify(e.snapshot());
test('Period production and duplicate clock calls cannot mint twice',()=>{
  const e=fresh();e.advance(30);assert.equal(e.state.stock.alliance.copper_ore,100);
  const before=stable(e);e.advance(30);assert.equal(stable(e),before);assert.equal(e.state.periods.length,2);
});
test('JSON restart preserves cursor and transaction receipts',()=>{
  const e=fresh();e.advance(30);e.reserve('transfer1','alliance','alliance_workshop',{copper_ore:10});
  e.deliver('receipt1','transfer1',{copper_ore:4});
  const r=new Economy(config,JSON.parse(JSON.stringify(e.snapshot())));
  const before=stable(r);r.advance(30);assert.equal(r.deliver('receipt1','transfer1',{copper_ore:4}),false);assert.equal(stable(r),before);
  r.deliver('receipt2','transfer1',{copper_ore:6});assert.equal(r.state.guildStock.alliance_workshop.copper_ore,10);r.audit();
});
test('Long outage caps catch-up and permanently forfeits old periods',()=>{
  const e=fresh();e.advance(600);assert.equal(e.state.stock.alliance.copper_ore,400);assert.equal(e.state.skippedPeriods,16);
  e.advance(600);assert.equal(e.state.stock.alliance.copper_ore,400);e.advance(630);assert.equal(e.state.stock.alliance.copper_ore,500);
});
test('Stock ceilings account for overflow explicitly',()=>{
  const c=JSON.parse(JSON.stringify(config));c.stockCaps.copper_ore=120;
  const e=new Economy(c);e.advance(60);assert.equal(e.state.stock.alliance.copper_ore,120);assert.equal(e.state.overflow.alliance.copper_ore,80);e.audit();
});
test('Upgrade costs atomically and applies only future output',()=>{
  const e=fresh();for(let p=1;p<=8;p++)e.advance(p*30);
  e.upgrade('upgrade1','alliance_pilot','alliance',240);assert.equal(e.state.stock.alliance.copper_ore,0);
  assert.equal(e.state.investment.alliance.copper_ore,800);assert.equal(e.state.periods.at(-2).tier,1);
  e.advance(270);assert.equal(e.state.stock.alliance.copper_ore,200);
  const before=stable(e);assert.equal(e.upgrade('upgrade1','alliance_pilot','alliance',270),false);assert.equal(stable(e),before);
});
test('Unaffordable upgrade spends no partial basket',()=>{
  const e=fresh();e.advance(30);const before=stable(e);
  assert.throws(()=>e.upgrade('upgrade1','alliance_pilot','alliance',30),/Insufficient/);assert.equal(stable(e),before);
});
test('Ownership loss stops future income and preserves existing goods',()=>{
  const e=fresh();e.setOwner('alliance_pilot',null,30);assert.equal(e.state.stock.alliance.copper_ore,100);
  e.advance(60);assert.equal(e.state.stock.alliance.copper_ore,100);
  e.setOwner('alliance_pilot','horde',60);e.advance(90);assert.equal(e.state.stock.horde.copper_ore,400);
});
test('Mid-period ownership and clock reversal refuse without mutations',()=>{
  const e=fresh();e.advance(30);const before=stable(e);
  assert.throws(()=>e.setOwner('alliance_pilot',null,45),/boundary/);assert.throws(()=>e.advance(0),/backwards/);assert.equal(stable(e),before);
});
test('Partial deliveries conserve in-transit stock and reject over-delivery',()=>{
  const e=fresh();e.advance(30);e.reserve('t1','alliance','alliance_workshop',{copper_ore:80});
  e.deliver('r1','t1',{copper_ore:30});const balance=e.audit().find(x=>x.faction==='alliance'&&x.resource==='copper_ore');
  assert.deepEqual([balance.factionHeld,balance.guildHeld,balance.inTransit],[20,30,50]);
  const before=stable(e);assert.throws(()=>e.deliver('r2','t1',{copper_ore:51}),/Insufficient/);assert.equal(stable(e),before);
});
test('Reservation replay refuses changed payload and prevents double spending',()=>{
  const e=fresh();e.advance(30);e.reserve('t1','alliance','alliance_workshop',{copper_ore:80});
  assert.equal(e.reserve('t1','alliance','alliance_workshop',{copper_ore:80}),false);
  const before=stable(e);assert.throws(()=>e.reserve('t1','alliance','alliance_workshop',{copper_ore:10}),/different payload/);
  assert.throws(()=>e.reserve('t2','alliance','alliance_workshop',{copper_ore:21}),/Insufficient/);assert.equal(stable(e),before);
});
test('Cross-faction allocation and foreign upgrade refuse',()=>{
  const e=fresh();e.advance(30);const before=stable(e);
  assert.throws(()=>e.reserve('t1','alliance','horde_workshop',{copper_ore:10}),/Cross-faction/);
  assert.throws(()=>e.upgrade('u1','horde_pilot','alliance',30),/not controlled/);assert.equal(stable(e),before);
});
test('Invalid quantities and prototype identifiers refuse',()=>{
  const e=fresh();e.advance(30);const before=stable(e);
  for(const q of [{copper_ore:-1},{copper_ore:0.5},{copper_ore:Number.MAX_SAFE_INTEGER+1},{gold:1},{}])
    assert.throws(()=>e.reserve('t1','alliance','alliance_workshop',q));
  assert.throws(()=>e.reserve('__proto__','alliance','alliance_workshop',{copper_ore:1}));assert.equal(stable(e),before);
});
test('Model batches consume once, without producing native progression',()=>{
  const e=fresh();e.advance(30);e.reserve('t1','alliance','alliance_workshop',{copper_ore:20});e.deliver('r1','t1',{copper_ore:20});
  e.completeModelBatch('b1','alliance_workshop',{copper_ore:20},'advancement');
  assert.equal(e.completeModelBatch('b1','alliance_workshop',{copper_ore:20},'advancement'),false);
  assert.equal(e.state.workshopConsumption.alliance.copper_ore,20);assert.equal(e.state.batches.b1.simulatedOnly,true);
  assert.equal(Object.hasOwn(e.state,'xp'),false);e.audit();
});
test('Tampered balances or mismatched config cannot restore',()=>{
  const e=fresh();e.advance(30);const saved=e.snapshot();saved.state.stock.alliance.copper_ore++;
  assert.throws(()=>new Economy(config,saved),/Conservation/);
  const c=JSON.parse(JSON.stringify(config));c.periodMinutes=60;assert.throws(()=>new Economy(c,e.snapshot()),/mismatch/);
});
test('Both policy projections conserve all resources across short, upgrade and 12-hour horizons',()=>{
  for(const policy of ['workshops','invest'])for(const h of [1,4,12]){
    const result=simulate(h,policy);assert.equal(result.balances.length,10);
    const row=result.timeline.at(-1);assert.deepEqual(row.alliance,row.horde);
    if(policy==='invest'&&h<4)assert.equal(row.alliance.equipmentBatches,0);
  }
});
const scenarios=[simulate(12,'workshops'),simulate(12,'invest')];
fs.writeFileSync(path.join(__dirname,'scenario-results.json'),JSON.stringify({offlineOnly:true,testCount:count,scenarios},null,2)+'\n');
console.log(`${count} tests passed; two illustrative scenarios saved.`);
