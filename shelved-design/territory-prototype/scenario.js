/* Illustrative balance inputs. Neither regional catalogue nor native recipe costs. */
(function (root) {
  'use strict';
  const { Economy } = typeof module !== 'undefined' && module.exports ? require('./economy.js') : root.AutoWowOfflineEconomy;
  const basket = { copper_ore:100, tin_ore:75, iron_ore:25, peacebloom:100, briarthorn:50 };
  const tier2 = Object.fromEntries(Object.entries(basket).map(([r, n]) => [r, n * 2]));
  const cost = { copper_ore:800, tin_ore:600, iron_ore:200, peacebloom:800, briarthorn:400 };
  const config = {
    resources:Object.keys(basket), factions:['alliance','horde'], periodMinutes:30, maxCatchUpPeriods:4,
    stockCaps:Object.fromEntries(Object.entries(basket).map(([r,n]) => [r,n*48])),
    guilds:{ alliance_workshop:'alliance', horde_workshop:'horde' },
    territories:{
      alliance_pilot:{ owner:'alliance', tiers:{1:{income:basket},2:{income:tier2,cost}} },
      horde_pilot:{ owner:'horde', tiers:{1:{income:basket},2:{income:tier2,cost}} }
    }
  };
  // Supply baskets only, intentionally not assigned to any WoW item or recipe.
  const equipment = { copper_ore:40, tin_ore:30, iron_ore:10, peacebloom:0, briarthorn:0 };
  const advancement = { copper_ore:20, tin_ore:15, iron_ore:5, peacebloom:20, briarthorn:10 };
  function simulate(hours = 12, policy = 'workshops') {
    if (!Number.isInteger(hours) || hours < 1 || hours > 48 || !['workshops','invest'].includes(policy)) throw new Error('Invalid scenario');
    const economy = new Economy(config); const timeline = [];
    for (let p=1;p<=hours*2;p++) {
      economy.advance(p*30);
      for (const faction of config.factions) {
        const territory = `${faction}_pilot`; const guild = `${faction}_workshop`;
        const t = economy.state.territories[territory];
        if (policy === 'invest' && t.tier === 1) {
          if (economy.canPay(economy.state.stock[faction], economy.quantities(cost))) economy.upgrade(`upgrade:${faction}`,territory,faction,p*30);
          // Saving consumes the first eight periods. Upgrade benefits start next period.
          continue;
        }
        // Bounded two advancement and four equipment batches per period; alternating prevents starvation.
        for (let round=0;round<4;round++) for (const kind of ['advancement','equipment']) {
          if (kind === 'advancement' && round >= 2) continue;
          const q = kind === 'advancement' ? advancement : equipment;
          if (!economy.canPay(economy.state.stock[faction], economy.quantities(q))) continue;
          const id=`${faction}:${p}:${kind}:${round}`;
          economy.reserve(id,faction,guild,q); economy.deliver(`receipt:${id}`,id,q);
          economy.completeModelBatch(`batch:${id}`,guild,q,kind);
        }
      }
      if (p%2===0) {
        const row = { hour:p/2 };
        for (const f of config.factions) {
          const batches=Object.values(economy.state.batches).filter(b=>b.request.guild===`${f}_workshop`);
          row[f]={ tier:economy.state.territories[`${f}_pilot`].tier, equipmentBatches:batches.filter(b=>b.request.kind==='equipment').length,
            advancementBatches:batches.filter(b=>b.request.kind==='advancement').length, copperHeld:economy.state.stock[f].copper_ore };
        }
        timeline.push(row);
      }
    }
    return { assumptions:'Illustrative equal baskets, doubled tier2, synthetic supply batch costs; no real crafts, skill gains, gear or reputation', hours, policy, timeline, balances:economy.audit(), snapshot:economy.snapshot() };
  }
  const api={config,simulate};
  if (typeof module !== 'undefined' && module.exports) module.exports=api;
  else root.AutoWowOfflineScenario=api;
})(typeof globalThis === 'undefined' ? window : globalThis);
