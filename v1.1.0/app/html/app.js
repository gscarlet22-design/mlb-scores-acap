(function () {
    'use strict';

    var API_BASE = '/local/mlb_scores/api';

    function api(path, opts) {
        return fetch(API_BASE + path, Object.assign({ credentials: 'same-origin' }, opts || {}));
    }

    /* ── Tabs ── */
    document.querySelectorAll('.tab').forEach(function (tab) {
        tab.addEventListener('click', function () {
            document.querySelectorAll('.tab').forEach(function (t) { t.classList.remove('active'); });
            document.querySelectorAll('.panel').forEach(function (p) { p.classList.remove('active'); });
            tab.classList.add('active');
            document.getElementById(tab.dataset.tab).classList.add('active');
        });
    });

    /* ── Team color map (id → {bg, fg}) ── */
    var teamColors = {};

    /* ── Clip list cache ── */
    var clipList = [];

    /* ── Tracked team rows (config tab) ── */
    /* Array of { team_id, notify_clip_id, score_clip_id } */
    var configTeams = [];

    /* ── Status: render scoreboard cards ── */
    function renderScoreboards(data) {
        var container = document.getElementById('scoreboards');
        container.innerHTML = '';

        if (!data.teams || data.teams.length === 0) {
            container.innerHTML = '<div class="no-teams-msg">No teams configured. Go to Config to add teams.</div>';
            return;
        }

        data.teams.forEach(function (t) {
            var card = document.createElement('div');
            card.className = 'score-card';

            var colors = teamColors[String(t.team_id)] || { bg: '#041E42', fg: '#FFFFFF' };
            var isLive  = t.game_state === 'Live';
            var isFinal = t.game_state === 'Final';
            var noGame  = !isLive && !isFinal && t.game_pk <= 0 && !t.next_game_opponent;

            if (isLive)  card.classList.add('live');
            if (isFinal) card.classList.add('final');
            if (noGame)  card.classList.add('no-game');

            /* State badge label */
            var stateText  = isLive ? 'LIVE' : isFinal ? 'Final' : (t.game_state || 'Scheduled');
            var stateCls   = isLive ? 'live-badge' : isFinal ? 'final-badge' : '';

            /* Score display */
            var hasScore = isLive || isFinal;
            var myScoreStr  = hasScore ? String(t.my_score)       : '&mdash;';
            var oppScoreStr = hasScore ? String(t.opponent_score)  : '&mdash;';
            var oppName     = t.opponent_name || 'Opponent';

            /* Middle section content */
            var midHtml = '';
            if (isLive) {
                midHtml =
                    '<div class="card-vs">vs</div>' +
                    '<div class="card-inning">' + (t.inning_state || '') + ' ' + (t.inning || '') + '</div>' +
                    '<div class="card-outs">' + t.outs + ' out' + (t.outs === 1 ? '' : 's') + '</div>';
            } else if (isFinal) {
                midHtml = '<div class="card-vs">Final</div>';
            } else if (t.next_game_opponent) {
                var vs = t.next_game_home ? 'vs' : '@';
                midHtml = '<div class="card-vs">' + vs + '</div>';
                oppName = t.next_game_opponent;
            } else {
                midHtml = '<div class="card-vs">&mdash;</div>';
            }

            /* Next game row */
            var nextHtml = '';
            if (!isLive && !isFinal && t.next_game_opponent) {
                nextHtml = '<div class="card-next">Next: ' +
                    (t.next_game_home ? 'vs' : '@') + ' ' + t.next_game_opponent +
                    ' &nbsp;|&nbsp; ' + t.next_game_date + ' ' + t.next_game_time +
                    '</div>';
            }
            if (!isLive && !isFinal && !t.next_game_opponent) {
                nextHtml = '<div class="card-next" style="color:#555">No games scheduled</div>';
            }

            /* Last play */
            var lastPlayHtml = '';
            if (isLive && t.last_play) {
                lastPlayHtml = '<div class="card-last-play">' +
                    t.last_play.substring(0, 120) + '</div>';
            }

            var pollTime = t.last_poll_time || '--';

            card.innerHTML =
                '<div class="card-header">' +
                    '<div class="card-team-name">' +
                        '<span class="team-swatch" style="background:' + colors.bg + ';border:1px solid ' + colors.fg + '"></span>' +
                        t.team_name +
                    '</div>' +
                    '<div class="card-state ' + stateCls + '">' + stateText + '</div>' +
                '</div>' +
                '<div class="card-scores">' +
                    '<div class="card-team-block">' +
                        '<div class="card-team-label">' + t.team_name + '</div>' +
                        '<div class="card-score">' + myScoreStr + '</div>' +
                    '</div>' +
                    '<div class="card-mid">' + midHtml + '</div>' +
                    '<div class="card-team-block">' +
                        '<div class="card-team-label">' + oppName + '</div>' +
                        '<div class="card-score">' + oppScoreStr + '</div>' +
                    '</div>' +
                '</div>' +
                nextHtml +
                lastPlayHtml +
                '<div style="font-size:10px;color:#444;margin-top:6px;text-align:right">polled ' + pollTime + '</div>';

            container.appendChild(card);
        });
    }

    /* ── Refresh status ── */
    function refreshStatus() {
        api('/status').then(function (r) { return r.json(); }).then(function (d) {
            renderScoreboards(d);
            var badge = document.getElementById('st-enabled');
            badge.textContent = d.enabled ? 'Enabled' : 'Disabled';
            badge.className   = 'badge ' + (d.enabled ? 'on' : 'off');
        }).catch(function () {});
    }

    refreshStatus();
    setInterval(refreshStatus, 10000);
    document.getElementById('btn-refresh').addEventListener('click', refreshStatus);

    /* ── Load all 30 teams into teamColors and the Add Team dropdown ── */
    function loadAllTeams(cb) {
        api('/teams').then(function (r) { return r.json(); }).then(function (d) {
            var teams = (d.teams || []).slice().sort(function (a, b) {
                return a.name.localeCompare(b.name);
            });
            teams.forEach(function (t) {
                teamColors[String(t.id)] = { bg: t.bg, fg: t.fg };
            });

            /* Populate Add Team dropdown */
            var sel = document.getElementById('add-team-select');
            sel.innerHTML = '<option value="">Select a team to add\u2026</option>';
            teams.forEach(function (t) {
                var opt = document.createElement('option');
                opt.value = t.id;
                opt.textContent = t.name + ' (' + t.abbr + ')';
                sel.appendChild(opt);
            });

            if (cb) cb();
        }).catch(function () { if (cb) cb(); });
    }

    /* ── Load clips into clipList ── */
    function loadClips(cb) {
        api('/clips').then(function (r) { return r.json(); }).then(function (d) {
            clipList = d.clips || [];
            if (cb) cb();
        }).catch(function () { if (cb) cb(); });
    }

    /* ── Build a clip <select> populated from clipList ── */
    function buildClipSelect(selectedId) {
        var sel = document.createElement('select');
        clipList.forEach(function (c) {
            var opt = document.createElement('option');
            opt.value = c.id;
            opt.textContent = c.name + ' (#' + c.id + ')';
            if (String(c.id) === String(selectedId)) opt.selected = true;
            sel.appendChild(opt);
        });
        return sel;
    }

    /* ── Render team rows in Config tab ── */
    function renderTeamRows() {
        var container = document.getElementById('team-rows');
        container.innerHTML = '';

        if (configTeams.length === 0) {
            container.innerHTML = '<p class="hint" style="margin-bottom:12px;">No teams added yet.</p>';
            return;
        }

        configTeams.forEach(function (tc, i) {
            var colors = teamColors[String(tc.team_id)] || { bg: '#041E42', fg: '#FFFFFF' };
            var t = getTeamMeta(tc.team_id);
            var teamLabel = t ? (t.name + ' (' + t.abbr + ')') : ('Team ' + tc.team_id);

            var row = document.createElement('div');
            row.className = 'team-row';
            row.dataset.idx = i;

            var header = document.createElement('div');
            header.className = 'team-row-header';

            var nameSpan = document.createElement('div');
            nameSpan.className = 'team-row-name';
            nameSpan.innerHTML =
                '<span class="team-swatch" style="background:' + colors.bg + ';border:1px solid ' + colors.fg + '"></span>' +
                teamLabel;

            var removeBtn = document.createElement('button');
            removeBtn.className = 'btn-remove';
            removeBtn.textContent = 'Remove';
            removeBtn.dataset.idx = i;
            removeBtn.addEventListener('click', function () {
                var idx = parseInt(this.dataset.idx);
                configTeams.splice(idx, 1);
                renderTeamRows();
            });

            header.appendChild(nameSpan);
            header.appendChild(removeBtn);

            var clips = document.createElement('div');
            clips.className = 'team-row-clips';

            /* Notify clip */
            var ngGroup = document.createElement('div');
            ngGroup.className = 'form-group';
            var ngLabel = document.createElement('label');
            ngLabel.textContent = 'Notify Sound';
            var notifySel = buildClipSelect(tc.notify_clip_id);
            notifySel.dataset.field = 'notify';
            notifySel.dataset.idx   = i;
            notifySel.addEventListener('change', function () {
                configTeams[parseInt(this.dataset.idx)].notify_clip_id = parseInt(this.value);
            });
            ngGroup.appendChild(ngLabel);
            ngGroup.appendChild(notifySel);

            /* Score clip */
            var sgGroup = document.createElement('div');
            sgGroup.className = 'form-group';
            var sgLabel = document.createElement('label');
            sgLabel.textContent = 'Score Sound';
            var scoreSel = buildClipSelect(tc.score_clip_id);
            scoreSel.dataset.field = 'score';
            scoreSel.dataset.idx   = i;
            scoreSel.addEventListener('change', function () {
                configTeams[parseInt(this.dataset.idx)].score_clip_id = parseInt(this.value);
            });
            sgGroup.appendChild(sgLabel);
            sgGroup.appendChild(scoreSel);

            clips.appendChild(ngGroup);
            clips.appendChild(sgGroup);

            row.appendChild(header);
            row.appendChild(clips);
            container.appendChild(row);
        });
    }

    /* ── Team meta helper (from teamColors keys — we need name/abbr too) ── */
    var allTeamsMeta = [];
    function getTeamMeta(id) {
        for (var i = 0; i < allTeamsMeta.length; i++)
            if (String(allTeamsMeta[i].id) === String(id)) return allTeamsMeta[i];
        return null;
    }

    /* ── Add Team button ── */
    document.getElementById('btn-add-team').addEventListener('click', function () {
        var sel = document.getElementById('add-team-select');
        var id  = parseInt(sel.value);
        if (!id) return;

        /* Prevent duplicates */
        for (var i = 0; i < configTeams.length; i++)
            if (configTeams[i].team_id === id) return;

        if (configTeams.length >= 8) {
            alert('Maximum of 8 teams supported.');
            return;
        }

        /* Default clips to first available */
        var defClip = clipList.length > 0 ? clipList[0].id : 38;
        configTeams.push({ team_id: id, notify_clip_id: defClip, score_clip_id: defClip });
        renderTeamRows();
        sel.value = '';
    });

    /* ── Load config ── */
    function loadConfig() {
        loadAllTeams(function () {
            /* After teams loaded, fetch clips then config */
            loadClips(function () {
                api('/config').then(function (r) { return r.json(); }).then(function (d) {
                    document.getElementById('enabled').checked       = (d.enabled || 'true').toLowerCase() === 'true';
                    document.getElementById('poll-interval').value   = d.poll_interval_sec || '30';
                    document.getElementById('device-user').value     = d.device_user || 'root';
                    document.getElementById('disp-enabled').checked  = (d.display_enabled || 'true').toLowerCase() === 'true';
                    document.getElementById('text-size').value       = d.display_text_size || 'large';
                    document.getElementById('text-color').value      = d.display_text_color || '#FFFFFF';
                    document.getElementById('bg-color').value        = d.display_bg_color || '#041E42';
                    document.getElementById('scroll-speed').value    = d.display_scroll_speed || '3';
                    document.getElementById('scroll-speed-val').textContent = d.display_scroll_speed || '3';
                    document.getElementById('persist-final').checked = (d.display_persist_final || 'true').toLowerCase() === 'true';
                    document.getElementById('duration').value        = Math.round((parseInt(d.display_duration_ms) || 20000) / 1000);

                    /* Build configTeams from response */
                    configTeams = [];
                    (d.teams || []).forEach(function (t) {
                        configTeams.push({
                            team_id:        parseInt(t.team_id)        || 0,
                            notify_clip_id: parseInt(t.notify_clip_id) || 38,
                            score_clip_id:  parseInt(t.score_clip_id)  || 38,
                        });
                    });
                    renderTeamRows();
                }).catch(function () { renderTeamRows(); });
            });
        });
    }
    loadConfig();

    /* Load allTeamsMeta for name lookups */
    api('/teams').then(function (r) { return r.json(); }).then(function (d) {
        allTeamsMeta = d.teams || [];
    }).catch(function () {});

    document.getElementById('scroll-speed').addEventListener('input', function () {
        document.getElementById('scroll-speed-val').textContent = this.value;
    });

    /* ── Save ── */
    function showMsg(text) {
        var el = document.getElementById('save-msg');
        el.textContent = text;
        setTimeout(function () { el.textContent = ''; }, 3000);
    }

    document.getElementById('btn-save').addEventListener('click', function () {
        var data = {
            enabled:               document.getElementById('enabled').checked ? 'true' : 'false',
            poll_interval_sec:     document.getElementById('poll-interval').value,
            device_user:           document.getElementById('device-user').value,
            display_enabled:       document.getElementById('disp-enabled').checked ? 'true' : 'false',
            display_text_size:     document.getElementById('text-size').value,
            display_text_color:    document.getElementById('text-color').value,
            display_bg_color:      document.getElementById('bg-color').value,
            display_scroll_speed:  document.getElementById('scroll-speed').value,
            display_persist_final: document.getElementById('persist-final').checked ? 'true' : 'false',
            display_duration_ms:   String(parseInt(document.getElementById('duration').value) * 1000),
            teams: configTeams.map(function (tc) {
                return {
                    team_id:        String(tc.team_id),
                    notify_clip_id: String(tc.notify_clip_id),
                    score_clip_id:  String(tc.score_clip_id),
                };
            }),
        };

        var pass = document.getElementById('device-pass').value;
        if (pass) data.device_pass = pass;

        api('/config', {
            method:  'POST',
            headers: { 'Content-Type': 'application/json' },
            body:    JSON.stringify(data),
        }).then(function (r) { return r.json(); })
          .then(function (d) {
              showMsg(d.message || 'Saved');
              setTimeout(refreshStatus, 500);
          })
          .catch(function () { showMsg('Save failed'); });

        document.getElementById('device-pass').value = '';
    });

    /* ── Test display ── */
    document.getElementById('btn-test-display').addEventListener('click', function () {
        api('/test_display', { method: 'POST' }).catch(function () {});
    });

})();
