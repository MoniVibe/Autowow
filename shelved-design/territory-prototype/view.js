(function () {
  'use strict';
  const root=document.getElementById('autowow-territory-strategy-20261008');
  const slider=root.querySelector('#autowow-strategy-hours');
  const label=root.querySelector('#autowow-strategy-hour-value');
  const comparison=root.querySelector('#autowow-strategy-comparison');
  const balances=root.querySelector('#autowow-strategy-balances');
  const detail=root.querySelector('#autowow-strategy-detail');
  const error=root.querySelector('#autowow-strategy-error');
  const names={copper_ore:'Copper ore',tin_ore:'Tin ore',iron_ore:'Iron ore',peacebloom:'Peacebloom',briarthorn:'Briarthorn'};
  const cache=new Map();
  function restore(state) {
    const hours=state && state.modelContent && state.modelContent.hours;
    if(Number.isInteger(hours)&&hours>=1&&hours<=24) slider.value=String(hours);
    render(false);
  }
  function render(save) {
    try {
      const hours=Number(slider.value);
      if(!Number.isInteger(hours)||hours<1||hours>24) throw new Error('Choose 1–24 hours');
      if(!cache.has(hours)) cache.set(hours,[AutoWowOfflineScenario.simulate(hours,'workshops'),AutoWowOfflineScenario.simulate(hours,'invest')]);
      const results=cache.get(hours);
      const [supply,invest]=results.map(r=>r.timeline.at(-1).alliance);
      label.textContent=`${hours} ${hours===1?'hour':'hours'}`;
      const rows=[['Territory tier',supply.tier,invest.tier],['Equipment supply batches',supply.equipmentBatches,invest.equipmentBatches],
        ['Advancement supply batches',supply.advancementBatches,invest.advancementBatches],['Copper ore held',supply.copperHeld,invest.copperHeld]];
      comparison.innerHTML=rows.map(([name,a,b])=>`<tr><th scope="row">${name}</th><td class="tabular-nums">${a}</td><td class="tabular-nums">${b}</td></tr>`).join('');
      detail.textContent=hours<=4?'Invest first withholds workshop supplies for four hours; tier 2 produces more from the next period.':
        `Invest first ${invest.equipmentBatches>=supply.equipmentBatches?'funds more equipment supply batches':'has not caught up on equipment supply batches'}; profession advancement still follows its separate queue.`;
      balances.innerHTML=results.flatMap((result,index)=>result.balances.filter(b=>b.faction==='alliance').map(b=>
        `<tr><th scope="row">${index===0?'Supply now':'Invest first'} / ${names[b.resource]}</th>`+
        [b.produced,b.workshopConsumed,b.investment,b.factionHeld+b.guildHeld+b.inTransit,b.overflow].map(n=>`<td class="tabular-nums">${n}</td>`).join('')+'</tr>')).join('');
      error.hidden=true;
      if(save && window.openai && window.openai.setWidgetState) {
        window.openai.setWidgetState({modelContent:{hours,scope:'Illustrative offline territorial economy; no live gear or resources'},privateContent:null}).catch(()=>{});
      }
    } catch(e) { error.textContent=e.message;error.hidden=false; }
  }
  slider.addEventListener('change',()=>render(true));
  window.addEventListener('openai:set_globals',event=>{
    if(event.detail && event.detail.globals && event.detail.globals.widgetState) restore(event.detail.globals.widgetState);
  });
  restore(window.openai && window.openai.widgetState);
})();
