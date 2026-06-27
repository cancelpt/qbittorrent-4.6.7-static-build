/*
 * Bittorrent Client using Qt and libtorrent.
 * Copyright (C) 2026
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

(function(root, factory) {
    const api = factory();

    if ((typeof module === "object") && module.exports)
        module.exports = api;

    if (typeof window !== "undefined") {
        window.qBittorrent ??= {};
        window.qBittorrent.TorrentListState = api;
    }
})(typeof globalThis !== "undefined" ? globalThis : this, function() {
    const normalizeInteger = (value, fallback = 0) => {
        const normalized = Number.parseInt(value, 10);
        return Number.isFinite(normalized) ? normalized : fallback;
    };

    const clamp = (value, min, max) => Math.min(Math.max(value, min), max);

    const buildRowOrderState = (rows) => {
        const rowIds = [];
        const rowIndexById = Object.create(null);
        const rowsById = Object.create(null);

        for (let index = 0; index < rows.length; ++index) {
            const row = rows[index];
            const rowId = `${row.rowId}`;

            rowIds.push(rowId);
            rowIndexById[rowId] = index;
            rowsById[rowId] = row;
        }

        return {
            rowIds,
            rowIndexById,
            rowsById
        };
    };

    const getRangeSelection = (state, rowId1, rowId2) => {
        const startIndex = state.rowIndexById[`${rowId1}`];
        const endIndex = state.rowIndexById[`${rowId2}`];

        if ((startIndex === undefined) || (endIndex === undefined))
            return [];

        const lowerIndex = Math.min(startIndex, endIndex);
        const upperIndex = Math.max(startIndex, endIndex);

        return state.rowIds.slice(lowerIndex, upperIndex + 1);
    };

    const getAdjacentRowId = (state, currentRowId, delta) => {
        const normalizedDelta = Math.sign(normalizeInteger(delta));
        if (normalizedDelta === 0)
            return null;

        const currentIndex = state.rowIndexById[`${currentRowId}`];

        if (currentIndex === undefined) {
            if (normalizedDelta > 0)
                return state.rowIds[0] ?? null;
            return null;
        }

        const nextIndex = currentIndex + normalizedDelta;
        if ((nextIndex < 0) || (nextIndex >= state.rowIds.length))
            return null;

        return state.rowIds[nextIndex];
    };

    const calculateWindowRange = ({totalRows, rowHeight, scrollTop, viewportHeight, overscan}) => {
        const safeTotalRows = Math.max(0, normalizeInteger(totalRows));
        const safeRowHeight = Math.max(1, normalizeInteger(rowHeight, 1));
        const safeScrollTop = Math.max(0, normalizeInteger(scrollTop));
        const safeViewportHeight = Math.max(0, normalizeInteger(viewportHeight));
        const safeOverscan = Math.max(0, normalizeInteger(overscan));

        if (safeTotalRows === 0) {
            return {
                startIndex: 0,
                endIndex: -1,
                topSpacerHeight: 0,
                bottomSpacerHeight: 0
            };
        }

        const firstVisibleIndex = clamp(Math.floor(safeScrollTop / safeRowHeight), 0, safeTotalRows - 1);
        const visibleRowCount = Math.max(1, Math.ceil(safeViewportHeight / safeRowHeight));
        const lastVisibleIndex = clamp(firstVisibleIndex + visibleRowCount - 1, firstVisibleIndex, safeTotalRows - 1);
        const startIndex = Math.max(0, firstVisibleIndex - safeOverscan);
        const endIndex = Math.min(safeTotalRows - 1, lastVisibleIndex + safeOverscan);

        return {
            startIndex,
            endIndex,
            topSpacerHeight: startIndex * safeRowHeight,
            bottomSpacerHeight: Math.max(0, (safeTotalRows - endIndex - 1) * safeRowHeight)
        };
    };

    return {
        buildRowOrderState,
        getRangeSelection,
        getAdjacentRowId,
        calculateWindowRange
    };
});
