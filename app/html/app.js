(function () {
    'use strict';

    var API_BASE = '/local/mlb_scores/api';

    function api(path, opts) {
        return fetch(API_BASE + path, Object.assign({ credentials: 'same-origin' }, opts || {}));
    }

    /* ---- Tabs ---- */
    document.querySelectorAll('.tab').forEach(function (tab) {
        tab.addEventListener('click', function () {
            document.querySelectorAll('.tab').forEach(function (t) { t.classList.remove('active'); });
            document.querySelectorAll('.panel').forEach(function (p) { p.classList.remove('active'); });
            tab.classList.add('active');
            document.getElementById(tab.dataset.tab).classList.add('active');
        });
    });

    /* ---- Status / Scoreboard ---- */
    function refreshStatus() {
        api('/status').then(function (r) { return r.json(); }).then(function (d) {
            var teamName = d.team_name || 'My Team';

            /* Team banner */
            document.getElementById('banner-team-name').textContent = teamName;
            document.getElementById('my-team-label').textContent    = teamName;

            /* Scores */
            var hasScore = d.is_live || d.game_state === 'Final';
            document.getElementById('my-score').textContent  = hasScore ? d.my_score    : '—';
            document.getElementById('opp-score').textContent = hasScore ? d.opponent_score : '—';
            document.getElementById('opp-name').textContent  = d.opponent_name || 'Opponent';

            /* Game meta */
            var stateEl = document.getElementById('game-state');
            var board   = document.getElementById('scoreboard');
            board.classList.remove('live', 'final', 'no-game');

            if (d.game_state === 'Live') {
                stateEl.textContent = 'LIVE';
                board.classList.add('live');
                document.getElementById('inning-info').textContent =
                    (d.inning_state || '') + ' ' + (d.inning || '');
                document.getElementById('outs-info').textContent =
                    d.outs + ' out' + (d.outs === 1 ? '' : 's');
            } else if (d.game_state === 'Final') {
                stateEl.textContent = 'Final';
                board.classList.add('final');
                document.getElementById('inning-info').textContent = '';
                document.getElementById('outs-info').textContent   = '';
            } else if (d.game_pk > 0) {
                stateEl.textContent = d.game_state || 'Scheduled';
                document.getElementById('inning-info').textContent = '';
                document.getElementById('outs-info').textContent   = '';
            } else {
                stateEl.textContent = 'No Game Today';
                board.classList.add('no-game');
                document.getElementById('inning-info').textContent = '';
                document.getElementById('outs-info').textContent   = '';
            }

            /* Last play */
            document.getElementById('last-play').textContent = d.last_play || '--';

            /* Status row */
            var badge = document.getElementById('st-enabled');
            badge.textContent = d.enabled ? 'Enabled' : 'Disabled';
            badge.className   = 'badge ' + (d.enabled ? 'on' : 'off');
            document.getElementById('st-last-poll').textContent = d.last_poll_time || '--';
        }).catch(function () {});
    }

    refreshStatus();
    setInterval(refreshStatus, 10000);
    document.getElementById('btn-refresh').addEventListener('click', refreshStatus);

    /* ---- Load team list from backend ---- */
    function loadTeams(selectedId) {
        api('/teams').then(function (r) { return r.json(); }).then(function (d) {
            var sel = document.getElementById('team-select');
            sel.innerHTML = '';
            /* Sort alphabetically by name */
            var teams = (d.teams || []).slice().sort(function (a, b) {
                return a.name.localeCompare(b.name);
            });
            teams.forEach(function (t) {
                var opt = document.createElement('option');
                opt.value = t.id;
                opt.textContent = t.name + ' (' + t.abbr + ')';
                if (selectedId && String(t.id) === String(selectedId)) opt.selected = true;
                sel.appendChild(opt);
            });
        }).catch(function () {});
    }

    /* ---- Load clips ---- */
    function loadClips(notifyId, scoreId) {
        api('/clips').then(function (r) { return r.json(); }).then(function (d) {
            ['clip-notify', 'clip-score'].forEach(function (selId, i) {
                var sel = document.getElementById(selId);
                sel.innerHTML = '';
                (d.clips || []).forEach(function (c) {
                    var opt = document.createElement('option');
                    opt.value       = c.id;
                    opt.textContent = c.name + ' (#' + c.id + ')';
                    var target = (i === 0) ? notifyId : scoreId;
                    if (target && String(c.id) === String(target)) opt.selected = true;
                    sel.appendChild(opt);
                });
            });
        }).catch(function () {});
    }

    /* ---- Load config ---- */
    function loadConfig() {
        api('/config').then(function (r) { return r.json(); }).then(function (d) {
            loadTeams(d.team_id);
            document.getElementById('enabled').checked       = (d.enabled || 'true').toLowerCase() === 'true';
            document.getElementById('poll-interval').value   = d.poll_interval_sec || '30';
            document.getElementById('device-user').value    = d.device_user || 'root';
            document.getElementById('disp-enabled').checked = (d.display_enabled || 'true').toLowerCase() === 'true';
            document.getElementById('text-size').value      = d.display_text_size || 'large';
            document.getElementById('text-color').value     = d.display_text_color || '#FFFFFF';
            document.getElementById('bg-color').value       = d.display_bg_color || '#041E42';
            document.getElementById('scroll-speed').value   = d.display_scroll_speed || '3';
            document.getElementById('scroll-speed-val').textContent = d.display_scroll_speed || '3';
            document.getElementById('duration').value       = Math.round((parseInt(d.display_duration_ms) || 20000) / 1000);
            loadClips(d.notification_clip_id, d.score_clip_id);
        }).catch(function () { loadTeams(); loadClips(); });
    }
    loadConfig();

    document.getElementById('scroll-speed').addEventListener('input', function () {
        document.getElementById('scroll-speed-val').textContent = this.value;
    });

    /* ---- Save ---- */
    function showMsg(text) {
        var el = document.getElementById('save-msg');
        el.textContent = text;
        setTimeout(function () { el.textContent = ''; }, 3000);
    }

    document.getElementById('btn-save').addEventListener('click', function () {
        var data = {
            team_id:              document.getElementById('team-select').value,
            enabled:              document.getElementById('enabled').checked ? 'true' : 'false',
            poll_interval_sec:    document.getElementById('poll-interval').value,
            notification_clip_id: document.getElementById('clip-notify').value,
            score_clip_id:        document.getElementById('clip-score').value,
            device_user:          document.getElementById('device-user').value,
            display_enabled:      document.getElementById('disp-enabled').checked ? 'true' : 'false',
            display_text_size:    document.getElementById('text-size').value,
            display_text_color:   document.getElementById('text-color').value,
            display_bg_color:     document.getElementById('bg-color').value,
            display_scroll_speed: document.getElementById('scroll-speed').value,
            display_duration_ms:  String(parseInt(document.getElementById('duration').value) * 1000)
        };
        var pass = document.getElementById('device-pass').value;
        if (pass) data.device_pass = pass;

        api('/config', {
            method:  'POST',
            headers: { 'Content-Type': 'application/json' },
            body:    JSON.stringify(data)
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              showMsg(d.message || 'Saved');
              /* Refresh status so team banner updates immediately */
              setTimeout(refreshStatus, 500);
          })
          .catch(function () { showMsg('Save failed'); });

        document.getElementById('device-pass').value = '';
    });

    /* ---- Test buttons ---- */
    document.getElementById('btn-test-display').addEventListener('click', function () {
        api('/test_display', { method: 'POST' }).catch(function () {});
    });
    document.getElementById('btn-test-audio').addEventListener('click', function () {
        api('/test_audio', { method: 'POST' }).catch(function () {});
    });

})();
