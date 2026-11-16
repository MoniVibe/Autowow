/* Offline territory ledger. No game/database/network/filesystem integration. */
(function (root) {
  'use strict';
  const copy = value => JSON.parse(JSON.stringify(value));
  function integer(value, label, min = 0) {
    if (!Number.isSafeInteger(value) || value < min) throw new Error(`Invalid ${label}`);
    return value;
  }
  function plus(a, b) { return integer(a + b, 'sum'); }
  class Economy {
    constructor(config, saved = null) {
      this.config = copy(config);
      this.resources = [...config.resources];
      this.factions = [...config.factions];
      if (new Set(this.resources).size !== this.resources.length || !this.resources.length ||
          new Set(this.factions).size !== this.factions.length || !this.factions.length)
        throw new Error('Duplicate/empty registry');
      integer(config.periodMinutes, 'period', 1);
      integer(config.maxCatchUpPeriods, 'catch-up limit', 1);
      for (const key of [...this.resources, ...this.factions, ...Object.keys(config.guilds), ...Object.keys(config.territories)])
        if (!/^[a-z][a-z0-9_]*$/.test(key)) throw new Error('Invalid registry key');
      this.quantities(config.stockCaps, true);
      for (const faction of Object.values(config.guilds)) this.requireFaction(faction);
      for (const territory of Object.values(config.territories)) {
        if (territory.owner !== null) this.requireFaction(territory.owner);
        if (!territory.tiers || !territory.tiers['1']) throw new Error('Missing initial tier');
        for (const [tier, spec] of Object.entries(territory.tiers)) {
          integer(Number(tier), 'tier', 1);
          this.quantities(spec.income, true);
          if (tier !== '1') this.quantities(spec.cost, true);
        }
      }
      const byFaction = () => Object.fromEntries(this.factions.map(f => [f, this.zero()]));
      this.state = saved ? copy(saved.state) : {
        lastPeriod: 0,
        skippedPeriods: 0,
        territories: Object.fromEntries(Object.entries(config.territories).map(([id, t]) => [id, { owner: t.owner, tier: 1 }])),
        stock: byFaction(),
        guildStock: Object.fromEntries(Object.keys(config.guilds).map(g => [g, this.zero()])),
        production: byFaction(), overflow: byFaction(), workshopConsumption: byFaction(), investment: byFaction(),
        transfers: {}, receipts: {}, batches: {}, upgrades: {}, periods: []
      };
      if (saved && (saved.version !== 1 || saved.configBinding !== JSON.stringify(this.config)))
        throw new Error('Snapshot/config mismatch');
      this.audit();
    }
    zero() { return Object.fromEntries(this.resources.map(r => [r, 0])); }
    requireFaction(f) { if (!this.factions.includes(f)) throw new Error('Unknown faction'); }
    requireGuild(g) { if (!Object.hasOwn(this.config.guilds, g)) throw new Error('Unknown guild'); }
    quantities(q, allowZero = false) {
      if (!q || typeof q !== 'object' || Array.isArray(q)) throw new Error('Invalid quantities');
      for (const [r, n] of Object.entries(q)) {
        if (!this.resources.includes(r)) throw new Error('Unknown resource');
        integer(n, 'quantity');
      }
      if (!allowZero && !Object.values(q).some(n => n > 0)) throw new Error('Empty transaction');
      return Object.assign(this.zero(), q);
    }
    id(id) { if (typeof id !== 'string' || !/^[a-zA-Z0-9][a-zA-Z0-9:_-]{0,99}$/.test(id)) throw new Error('Invalid transaction id'); }
    replay(table, id, request) {
      this.id(id);
      if (!Object.hasOwn(table, id)) return false;
      if (JSON.stringify(table[id].request) !== JSON.stringify(request)) throw new Error('Transaction id reused with different payload');
      return true;
    }
    canPay(stock, q) { return this.resources.every(r => stock[r] >= q[r]); }
    move(from, to, q) {
      if (!this.canPay(from, q)) throw new Error('Insufficient stock');
      // Check every destination before mutating either side.
      for (const r of this.resources) plus(to[r], q[r]);
      for (const r of this.resources) { from[r] -= q[r]; to[r] += q[r]; }
    }
    advance(nowMinutes) {
      integer(nowMinutes, 'elapsed minutes');
      const completed = Math.floor(nowMinutes / this.config.periodMinutes);
      if (completed < this.state.lastPeriod) throw new Error('Clock moved backwards');
      const first = Math.max(this.state.lastPeriod + 1, completed - this.config.maxCatchUpPeriods + 1);
      this.state.skippedPeriods = plus(this.state.skippedPeriods, Math.max(0, first - this.state.lastPeriod - 1));
      for (let period = first; period <= completed; period++) {
        for (const [id, t] of Object.entries(this.state.territories)) {
          if (t.owner === null) continue;
          const income = this.quantities(this.config.territories[id].tiers[t.tier].income, true);
          const stock = this.state.stock[t.owner];
          const applied = this.zero(); const spilled = this.zero();
          // Precheck safe arithmetic before any period mutation.
          for (const r of this.resources) {
            applied[r] = Math.min(income[r], Math.max(0, (this.config.stockCaps[r] || 0) - stock[r]));
            spilled[r] = income[r] - applied[r];
            plus(this.state.production[t.owner][r], income[r]); plus(this.state.overflow[t.owner][r], spilled[r]);
          }
          for (const r of this.resources) {
            stock[r] += applied[r]; this.state.production[t.owner][r] += income[r]; this.state.overflow[t.owner][r] += spilled[r];
          }
          this.state.periods.push({ id: `${id}:${period}`, territory: id, period, owner: t.owner, tier: t.tier, credited: applied, overflow: spilled });
        }
      }
      this.state.lastPeriod = completed; // Skipped outage periods are forfeited, never replayed later.
      this.audit();
    }
    setOwner(territory, owner, nowMinutes) {
      if (!Object.hasOwn(this.state.territories, territory)) throw new Error('Unknown territory');
      if (owner !== null) this.requireFaction(owner);
      integer(nowMinutes, 'ownership time');
      if (nowMinutes % this.config.periodMinutes !== 0) throw new Error('Ownership changes require a period boundary');
      this.advance(nowMinutes); // Completed periods stay with the old owner.
      this.state.territories[territory].owner = owner;
    }
    upgrade(id, territory, faction, nowMinutes) {
      this.requireFaction(faction);
      const request = { territory, faction };
      if (this.replay(this.state.upgrades, id, request)) return false;
      const t = this.state.territories[territory];
      if (!t || t.owner !== faction) throw new Error('Territory not controlled');
      const spec = this.config.territories[territory].tiers[t.tier + 1];
      if (!spec) throw new Error('No further tier');
      integer(nowMinutes, 'upgrade time');
      if (nowMinutes % this.config.periodMinutes !== 0) throw new Error('Upgrades require a period boundary');
      this.advance(nowMinutes); // Never apply a new tier to an unsettled past period.
      const cost = this.quantities(spec.cost);
      this.move(this.state.stock[faction], this.state.investment[faction], cost);
      t.tier++;
      this.state.upgrades[id] = { request, tier: t.tier, cost, afterPeriod: this.state.lastPeriod };
      this.audit(); return true;
    }
    reserve(id, faction, guild, quantities) {
      this.requireFaction(faction); this.requireGuild(guild);
      if (this.config.guilds[guild] !== faction) throw new Error('Cross-faction allocation');
      const q = this.quantities(quantities); const request = { faction, guild, quantities: q };
      if (this.replay(this.state.transfers, id, request)) return false;
      const outstanding = this.zero();
      this.move(this.state.stock[faction], outstanding, q);
      this.state.transfers[id] = { request, outstanding, delivered: this.zero() };
      this.audit(); return true;
    }
    deliver(receiptId, transferId, quantities) {
      const q = this.quantities(quantities); const request = { transferId, quantities: q };
      if (this.replay(this.state.receipts, receiptId, request)) return false;
      const transfer = this.state.transfers[transferId];
      if (!transfer) throw new Error('Unknown transfer');
      for (const r of this.resources) plus(transfer.delivered[r], q[r]);
      this.move(transfer.outstanding, this.state.guildStock[transfer.request.guild], q);
      for (const r of this.resources) transfer.delivered[r] += q[r];
      this.state.receipts[receiptId] = { request };
      this.audit(); return true;
    }
    completeModelBatch(id, guild, quantities, kind) {
      this.requireGuild(guild);
      if (!['equipment', 'advancement'].includes(kind)) throw new Error('Unknown queue');
      const q = this.quantities(quantities); const request = { guild, quantities: q, kind };
      if (this.replay(this.state.batches, id, request)) return false;
      this.move(this.state.guildStock[guild], this.state.workshopConsumption[this.config.guilds[guild]], q);
      this.state.batches[id] = { request, simulatedOnly: true };
      this.audit(); return true;
    }
    audit() {
      integer(this.state.lastPeriod, 'last period'); integer(this.state.skippedPeriods, 'skipped periods');
      for (const [id, t] of Object.entries(this.state.territories)) {
        if (!Object.hasOwn(this.config.territories, id) || !this.config.territories[id].tiers[t.tier]) throw new Error('Invalid territory state');
        if (t.owner !== null) this.requireFaction(t.owner);
      }
      if (Object.keys(this.state.territories).length !== Object.keys(this.config.territories).length) throw new Error('Missing territory');
      for (const [g, stock] of Object.entries(this.state.guildStock)) { this.requireGuild(g); this.quantities(stock, true); }
      if (Object.keys(this.state.guildStock).length !== Object.keys(this.config.guilds).length) throw new Error('Missing guild');
      for (const table of ['stock','production','overflow','workshopConsumption','investment']) {
        if (Object.keys(this.state[table]).length !== this.factions.length) throw new Error('Missing faction account');
        for (const [f, q] of Object.entries(this.state[table])) { this.requireFaction(f); this.quantities(q, true); }
      }
      for (const tr of Object.values(this.state.transfers)) {
        this.requireGuild(tr.request.guild); this.requireFaction(tr.request.faction);
        if (this.config.guilds[tr.request.guild] !== tr.request.faction) throw new Error('Invalid transfer faction');
        for (const key of ['outstanding','delivered']) this.quantities(tr[key], true);
        const q = this.quantities(tr.request.quantities);
        for (const r of this.resources) if (tr.outstanding[r] + tr.delivered[r] !== q[r]) throw new Error('Transfer does not reconcile');
      }
      const balances = [];
      for (const f of this.factions) for (const r of this.resources) {
        const guildHeld = Object.entries(this.state.guildStock).filter(([g]) => this.config.guilds[g] === f).reduce((n, [, q]) => plus(n, q[r]), 0);
        const inTransit = Object.values(this.state.transfers).filter(tr => tr.request.faction === f).reduce((n, tr) => plus(n, tr.outstanding[r]), 0);
        const accounted = [this.state.stock[f][r], guildHeld, inTransit, this.state.workshopConsumption[f][r], this.state.investment[f][r], this.state.overflow[f][r]].reduce(plus, 0);
        if (accounted !== this.state.production[f][r]) throw new Error(`Conservation failure: ${f}/${r}`);
        balances.push({ faction: f, resource: r, produced: this.state.production[f][r], factionHeld: this.state.stock[f][r], guildHeld, inTransit, workshopConsumed: this.state.workshopConsumption[f][r], investment: this.state.investment[f][r], overflow: this.state.overflow[f][r] });
      }
      return balances;
    }
    snapshot() { this.audit(); return { version: 1, configBinding: JSON.stringify(this.config), state: copy(this.state) }; }
  }
  const api = { Economy };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.AutoWowOfflineEconomy = api;
})(typeof globalThis === 'undefined' ? window : globalThis);
