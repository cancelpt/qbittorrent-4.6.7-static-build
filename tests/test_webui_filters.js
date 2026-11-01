// Regression tests exercise the functions shipped in the WebUI overlay.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

const scripts = path.join(__dirname, '../packaging/qbt-static/patches/qbittorrent/4.6.7/source/src/webui/www/private/scripts');
const tableSource = fs.readFileSync(path.join(scripts, 'dynamicTable.js'), 'utf8');
const clientSource = fs.readFileSync(path.join(scripts, 'client.js'), 'utf8');
const filterMatch = tableSource.match(/applyFilter: function\(([^)]*)\) \{([\s\S]*?)\n        \},\n\n        getFilteredTorrentsNumber:/);
assert.ok(filterMatch, 'the shipped torrent filter must be present');

function context(subcategories = true) {
    const scope = vm.createContext({
        CATEGORIES_ALL: 1, CATEGORIES_UNCATEGORIZED: 2,
        TAGS_ALL: 1, TAGS_UNTAGGED: 2,
        TRACKERS_ALL: 1, TRACKERS_TRACKERLESS: 2,
        TRACKERS_ERROR: 3, TRACKERS_WARNING: 4, TRACKERS_ANNOUNCE_ERROR: 5,
        category_list: {101: {name: 'example'}, 102: {name: 'example/child'}},
        useSubcategories: subcategories,
        genHash: name => ({example: 101, 'example/child': 102, unrelated: 103}[name]),
        trackerList: new Map(),
        window: {qBittorrent: {Misc: {containsAllTerms: (name, terms) => terms.every(term => name.includes(term))}}},
        stored: {},
        LocalPreferences: {set(key, value) { scope.stored[key] = value; }}
    });
    vm.runInContext(`applyFilter = function(${filterMatch[1]}) {${filterMatch[2]}\n};`, scope);
    return scope;
}

function row(category = 'example', trackerState = 0) {
    return {full_data: {state: 'stalledUP', name: 'Example torrent', category, tags: '', trackers_count: 1, tracker_state: trackerState}};
}

for (const subcategories of [true, false]) {
    test(`stale category does not crash or hide the list (subcategories=${subcategories})`, () => {
        const scope = context(subcategories);
        assert.equal(scope.applyFilter(row(), 'all', 999, 1, 1), true);
        assert.equal(scope.applyFilter(row(), 'all', '999', 1, 1), true);
    });
    test(`existing category filters remain effective (subcategories=${subcategories})`, () => {
        const scope = context(subcategories);
        assert.equal(scope.applyFilter(row(), 'all', 101, 1, 1), true);
        assert.equal(scope.applyFilter(row('unrelated'), 'all', 101, 1, 1), false);
        assert.equal(scope.applyFilter(row('example/child'), 'all', 101, 1, 1), subcategories);
        assert.equal(scope.applyFilter(row(''), 'all', 2, 1, 1), true);
        assert.equal(scope.applyFilter(row(), 'all', 2, 1, 1), false);
    });
}

test('stale category fallback still applies the other filters', () => {
    const scope = context();
    assert.equal(scope.applyFilter(row(), 'downloading', 999, 1, 1), false);
    assert.equal(scope.applyFilter(row(), 'all', 999, 1, 1, ['absent']), false);
});

test('invalid saved category is repaired after category data arrives', () => {
    const match = clientSource.match(/const normalizeCategoryFilter = function\(\) \{([\s\S]*?)\n    \};/);
    assert.ok(match, 'saved-category recovery must be present');
    const scope = context();
    vm.runInContext(`normalizeCategoryFilter = function() {${match[1]}\n};`, scope);
    for (const saved of [999, '999', 'invalid', '__proto__', '', null]) {
        scope.selected_category = saved;
        assert.equal(scope.normalizeCategoryFilter(), true);
        assert.equal(scope.selected_category, 1);
        assert.equal(scope.stored.selected_category, 1);
    }
    for (const saved of [1, '1', 2, '2', 101, '101']) {
        scope.selected_category = saved;
        assert.equal(scope.normalizeCategoryFilter(), false);
        assert.equal(scope.selected_category, saved);
    }
    const recovery = clientSource.indexOf('if (normalizeCategoryFilter())');
    const tableUpdate = clientSource.indexOf('torrentsTable.updateTable(full_update);');
    assert.ok(recovery > clientSource.indexOf("if (response['torrents_removed'])"));
    assert.ok(recovery < tableUpdate, 'repair must run before the first table render');
});

test('tracker status filters select their corresponding bits', () => {
    const scope = context();
    for (let state = 0; state < 8; ++state) {
        for (const [filter, bit] of [[3, 2], [4, 1], [5, 4]])
            assert.equal(scope.applyFilter(row('example', state), 'all', 1, 1, filter), Boolean(state & bit));
    }
});
