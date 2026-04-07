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
    /* Each entry: { team_id, notify_clip_id, score_clip_id,
                     text_color, bg_color, text_size, scroll_speed, duration_ms } */
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
            var isPreview = !isLive && !isFinal &&
                            (t.game_state === 'Preview' || t.game_state === 'Pre-Game' ||
                             t.game_state === 'Warmup'  || t.game_state === 'Scheduled');
            var nextHtml = '';
            if (!isLive && !isFinal && t.next_game_opponent) {
                var gameLabel = isPreview ? 'Today:' : 'Next:';
                nextHtml = '<div class="card-next">' + gameLabel + ' ' +
                    (t.next_game_home ? 'vs' : '@') + ' ' + t.next_game_opponent +
                    ' &nbsp;|&nbsp; ' + t.next_game_date + ' ' + t.next_game_time +
                    '</div>';
            }
            if (!isLive && !isFinal && !t.next_game_opponent) {
                nextHtml = '<div class="card-next" style="color:#555">No games scheduled</div>';
            }

            /* Last play — show whenever we have text (live or final) */
            var lastPlayHtml = '';
            if (t.last_play) {
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

            /* ── Header: team name + Remove button ── */
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

            /* ── Audio row: Notify clip + Score clip + Test Audio button ── */
            var audioRow = document.createElement('div');
            audioRow.className = 'team-row-audio';

            var ngGroup = document.createElement('div');
            ngGroup.className = 'form-group';
            var ngLabel = document.createElement('label');
            ngLabel.textContent = 'Inning Change Sound';
            var notifySel = buildClipSelect(tc.notify_clip_id);
            notifySel.dataset.field = 'notify';
            notifySel.dataset.idx   = i;
            notifySel.addEventListener('change', function () {
                configTeams[parseInt(this.dataset.idx)].notify_clip_id = parseInt(this.value);
            });
            ngGroup.appendChild(ngLabel);
            ngGroup.appendChild(notifySel);

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

            /* Audio enabled toggle */
            var aeGroup = document.createElement('div');
            aeGroup.className = 'form-group audio-enabled-group';
            var aeLabel = document.createElement('label');
            aeLabel.textContent = 'Audio';
            var aeToggle = document.createElement('label');
            aeToggle.className = 'toggle-switch';
            var aeCheck = document.createElement('input');
            aeCheck.type = 'checkbox';
            aeCheck.checked = tc.audio_enabled !== false && tc.audio_enabled !== 0;
            (function (idx) {
                aeCheck.addEventListener('change', function () {
                    configTeams[idx].audio_enabled = this.checked;
                });
            })(i);
            var aeSlider = document.createElement('span');
            aeSlider.className = 'toggle-slider';
            aeToggle.appendChild(aeCheck);
            aeToggle.appendChild(aeSlider);
            aeGroup.appendChild(aeLabel);
            aeGroup.appendChild(aeToggle);

            /* Test Audio button — plays this team's notify clip */
            var testAudioBtn = document.createElement('button');
            testAudioBtn.className = 'btn-test-audio';
            testAudioBtn.textContent = 'Test';
            testAudioBtn.title = 'Play this team\'s notify clip';
            (function (teamId) {
                testAudioBtn.addEventListener('click', function () {
                    api('/test_audio', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ team_id: teamId }),
                    }).catch(function () {});
                });
            })(tc.team_id);

            audioRow.appendChild(aeGroup);
            audioRow.appendChild(ngGroup);
            audioRow.appendChild(sgGroup);
            audioRow.appendChild(testAudioBtn);

            /* ── Display settings row ── */
            var dispRow = document.createElement('div');
            dispRow.className = 'team-row-display';

            /* Text Color */
            var tcGroup = document.createElement('div');
            tcGroup.className = 'form-group half';
            var tcLabel = document.createElement('label');
            tcLabel.textContent = 'Text Color';
            var tcInput = document.createElement('input');
            tcInput.type = 'color';
            tcInput.value = tc.text_color || '#FFFFFF';
            tcInput.addEventListener('input', function () {
                configTeams[i].text_color = this.value;
            });
            tcGroup.appendChild(tcLabel);
            tcGroup.appendChild(tcInput);

            /* Background Color */
            var bcGroup = document.createElement('div');
            bcGroup.className = 'form-group half';
            var bcLabel = document.createElement('label');
            bcLabel.textContent = 'Background';
            var bcInput = document.createElement('input');
            bcInput.type = 'color';
            bcInput.value = tc.bg_color || '#041E42';
            bcInput.addEventListener('input', function () {
                configTeams[i].bg_color = this.value;
            });
            bcGroup.appendChild(bcLabel);
            bcGroup.appendChild(bcInput);

            /* Text Size */
            var tsGroup = document.createElement('div');
            tsGroup.className = 'form-group third';
            var tsLabel = document.createElement('label');
            tsLabel.textContent = 'Text Size';
            var tsSel = document.createElement('select');
            ['small', 'medium', 'large'].forEach(function (sz) {
                var opt = document.createElement('option');
                opt.value = sz;
                opt.textContent = sz.charAt(0).toUpperCase() + sz.slice(1);
                if (sz === (tc.text_size || 'large')) opt.selected = true;
                tsSel.appendChild(opt);
            });
            tsSel.addEventListener('change', function () {
                configTeams[i].text_size = this.value;
            });
            tsGroup.appendChild(tsLabel);
            tsGroup.appendChild(tsSel);

            /* Scroll Speed */
            var ssGroup = document.createElement('div');
            ssGroup.className = 'form-group third';
            var ssLabel = document.createElement('label');
            ssLabel.textContent = 'Speed';
            var ssInput = document.createElement('input');
            ssInput.type = 'range'; ssInput.min = 1; ssInput.max = 5; ssInput.step = 1;
            ssInput.value = String(tc.scroll_speed || 3);
            var ssVal = document.createElement('span');
            ssVal.className = 'range-val';
            ssVal.textContent = ssInput.value;
            ssInput.addEventListener('input', function () {
                ssVal.textContent = this.value;
                configTeams[i].scroll_speed = parseInt(this.value);
            });
            ssGroup.appendChild(ssLabel);
            ssGroup.appendChild(ssInput);
            ssGroup.appendChild(ssVal);

            /* Duration */
            var dmGroup = document.createElement('div');
            dmGroup.className = 'form-group third';
            var dmLabel = document.createElement('label');
            dmLabel.textContent = 'Duration (s)';
            var dmInput = document.createElement('input');
            dmInput.type = 'number'; dmInput.min = 3; dmInput.max = 120;
            dmInput.value = String(Math.round((tc.duration_ms || 20000) / 1000));
            dmInput.addEventListener('change', function () {
                configTeams[i].duration_ms = parseInt(this.value) * 1000;
            });
            dmGroup.appendChild(dmLabel);
            dmGroup.appendChild(dmInput);

            dispRow.appendChild(tcGroup);
            dispRow.appendChild(bcGroup);
            dispRow.appendChild(tsGroup);
            dispRow.appendChild(ssGroup);
            dispRow.appendChild(dmGroup);

            row.appendChild(header);
            row.appendChild(audioRow);
            row.appendChild(dispRow);
            container.appendChild(row);
        });
    }

    /* ── Team meta helper ── */
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

        /* Default clips to first available; display to team's official colors */
        var defClip = clipList.length > 0 ? clipList[0].id : 38;
        var tc      = teamColors[String(id)] || { bg: '#041E42', fg: '#FFFFFF' };
        configTeams.push({
            team_id:        id,
            notify_clip_id: defClip,
            score_clip_id:  defClip,
            audio_enabled:  true,
            text_color:     tc.fg,
            bg_color:       tc.bg,
            text_size:      'large',
            scroll_speed:   3,
            duration_ms:    20000,
        });
        renderTeamRows();
        sel.value = '';
    });

    /* ── Load config ── */
    function loadConfig() {
        loadAllTeams(function () {
            loadClips(function () {
                api('/config').then(function (r) { return r.json(); }).then(function (d) {
                    document.getElementById('enabled').checked       = (d.enabled || 'true').toLowerCase() === 'true';
                    document.getElementById('poll-interval').value   = d.poll_interval_sec || '30';
                    document.getElementById('device-user').value     = d.device_user || 'root';
                    document.getElementById('disp-enabled').checked  = (d.display_enabled || 'true').toLowerCase() === 'true';
                    document.getElementById('persist-final').checked = (d.display_persist_final || 'true').toLowerCase() === 'true';
                    document.getElementById('strobe-enabled').checked = d.strobe_enabled !== false && d.strobe_enabled !== 0;
                    var vol = parseInt(d.audio_volume) || 75;
                    document.getElementById('audio-volume').value     = vol;
                    document.getElementById('audio-volume-val').textContent = vol;

                    /* Build configTeams from response (includes per-team display settings) */
                    configTeams = [];
                    (d.teams || []).forEach(function (t) {
                        var tc = teamColors[String(t.team_id)] || { bg: '#041E42', fg: '#FFFFFF' };
                        configTeams.push({
                            team_id:        parseInt(t.team_id)        || 0,
                            notify_clip_id: parseInt(t.notify_clip_id) || 38,
                            score_clip_id:  parseInt(t.score_clip_id)  || 38,
                            audio_enabled:  t.audio_enabled !== false && t.audio_enabled !== 0,
                            text_color:     t.text_color  || tc.fg,
                            bg_color:       t.bg_color    || tc.bg,
                            text_size:      t.text_size   || 'large',
                            scroll_speed:   parseInt(t.scroll_speed) || 3,
                            duration_ms:    parseInt(t.duration_ms)  || 20000,
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

    /* ── Save ── */
    function showMsg(text) {
        var el = document.getElementById('save-msg');
        el.textContent = text;
        setTimeout(function () { el.textContent = ''; }, 3000);
    }

    document.getElementById('audio-volume').addEventListener('input', function () {
        document.getElementById('audio-volume-val').textContent = this.value;
    });
    document.getElementById('audio-volume').addEventListener('change', function () {
        api('/config', {
            method:  'POST',
            headers: { 'Content-Type': 'application/json' },
            body:    JSON.stringify({ audio_volume: this.value }),
        }).catch(function () {});
    });

    document.getElementById('btn-save').addEventListener('click', function () {
        var data = {
            enabled:               document.getElementById('enabled').checked ? 'true' : 'false',
            poll_interval_sec:     document.getElementById('poll-interval').value,
            device_user:           document.getElementById('device-user').value,
            audio_volume:          document.getElementById('audio-volume').value,
            display_enabled:       document.getElementById('disp-enabled').checked ? 'true' : 'false',
            display_persist_final: document.getElementById('persist-final').checked ? 'true' : 'false',
            strobe_enabled:        document.getElementById('strobe-enabled').checked,
            teams: configTeams.map(function (tc) {
                return {
                    team_id:        String(tc.team_id),
                    notify_clip_id: String(tc.notify_clip_id),
                    score_clip_id:  String(tc.score_clip_id),
                    audio_enabled:  tc.audio_enabled !== false && tc.audio_enabled !== 0,
                    text_color:     tc.text_color  || '#FFFFFF',
                    bg_color:       tc.bg_color    || '#041E42',
                    text_size:      tc.text_size   || 'large',
                    scroll_speed:   String(tc.scroll_speed || 3),
                    duration_ms:    String(tc.duration_ms  || 20000),
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

    /* ── Install bundled clips ── */
    document.getElementById('btn-install-clips').addEventListener('click', function () {
        var btn = this;
        var out = document.getElementById('install-clips-out');
        btn.disabled = true;
        btn.textContent = 'Installing…';
        out.textContent = '';
        api('/upload_clips', { method: 'POST' })
            .then(function (r) { return r.json(); })
            .then(function (d) {
                var lines = (d.clips || []).map(function (c) {
                    var ok = c.http_code >= 200 && c.http_code < 300;
                    return (ok ? '✓' : '✗') + ' ' + c.name +
                           ' — HTTP ' + c.http_code + '\n  ' + c.response;
                });
                out.textContent = lines.join('\n\n') +
                    '\n\nClip list refreshed — assign clips to teams above.';
                /* Refresh clip dropdowns so new clips appear */
                loadClips(function () { renderTeamRows(); });
            })
            .catch(function (e) { out.textContent = 'Request failed: ' + e; })
            .finally(function () {
                btn.disabled = false;
                btn.textContent = 'Install Default Clips onto Device';
            });
    });

    /* ── Volume diagnostics ── */
    document.getElementById('btn-volume-diag').addEventListener('click', function () {
        var btn = this;
        var out = document.getElementById('volume-diag-out');
        btn.disabled = true;
        btn.textContent = 'Running…';
        out.textContent = '';
        api('/volume_diag').then(function (r) { return r.json(); }).then(function (d) {
            var s1 = d.step1_download  || {};
            var s2 = d.step2_transmit  || {};
            var lines = [
                'Volume: ' + d.volume_pct + '%   Pipeline: ' + d.pipeline,
                '',
                '── Step 1: Download clip as .au ─────',
                'Clip ID: ' + s1.clip_id + '   URL: ' + s1.url,
                'HTTP: ' + s1.http_code + '   CURL: ' + s1.curl_code + ' (' + s1.curl_error + ')',
                'Bytes received: ' + s1.bytes_received,
                'Magic: ' + s1.magic_hex + '   Format detected: ' + s1.format_detected,
                'Encoding: ' + s1.au_encoding + ' (' + s1.au_encoding_name + ')   Rate: ' + s1.au_sample_rate + ' Hz   Ch: ' + s1.au_channels,
                'Data offset: ' + s1.au_data_offset + '   Decode supported: ' + s1.decode_supported,
                s1.response_preview ? ('Response preview: ' + s1.response_preview) : '',
                '',
                '── Step 2: POST to transmit.cgi ─────',
                'Data source: ' + s2.data_source + '   Bytes: ' + s2.bytes_sent,
                'HTTP: ' + s2.http_code + '   CURL: ' + s2.curl_code + ' (' + s2.curl_error + ')',
                'Response: ' + s2.response,
                'SUCCESS: ' + s2.success,
                '',
                '── Device Audio Parameters ──────────',
                d.audio_params,
            ].filter(function(l){ return l !== undefined; });
            out.textContent = lines.join('\n');
        }).catch(function (e) {
            out.textContent = 'Request failed: ' + e;
        }).finally(function () {
            btn.disabled = false;
            btn.textContent = 'Run Diagnostics';
        });
    });

    /* ── Schedule ── */
    var scheduleFilter   = 'all';
    var lastScheduleData = null;

    document.getElementById('sched-btn-all').addEventListener('click', function () {
        scheduleFilter = 'all';
        document.getElementById('sched-btn-all').classList.add('active');
        document.getElementById('sched-btn-mine').classList.remove('active');
        if (lastScheduleData) renderSchedule(lastScheduleData);
    });
    document.getElementById('sched-btn-mine').addEventListener('click', function () {
        scheduleFilter = 'my_teams';
        document.getElementById('sched-btn-mine').classList.add('active');
        document.getElementById('sched-btn-all').classList.remove('active');
        if (lastScheduleData) renderSchedule(lastScheduleData);
    });

    function renderSchedule(data) {
        lastScheduleData = data;
        var container = document.getElementById('schedule');
        container.innerHTML = '';
        var days = data.days || [];

        /* Build my-teams name set when filter is active */
        var myTeamNames = null;
        if (scheduleFilter === 'my_teams') {
            myTeamNames = {};
            configTeams.forEach(function (tc) {
                var meta = getTeamMeta(tc.team_id);
                if (meta) myTeamNames[meta.name] = true;
            });
        }

        /* Filter days/games */
        var filteredDays = days.map(function (day) {
            if (!myTeamNames) return day;
            var games = (day.games || []).filter(function (g) {
                return myTeamNames[g.away] || myTeamNames[g.home];
            });
            return { date: day.date, games: games };
        }).filter(function (day) { return day.games.length > 0; });

        if (filteredDays.length === 0) {
            var msg = scheduleFilter === 'my_teams'
                ? (configTeams.length === 0 ? 'No teams configured.' : 'No games for monitored teams this week.')
                : 'No games found this week.';
            container.innerHTML = '<div class="schedule-empty">' + msg + '</div>';
            return;
        }

        filteredDays.forEach(function (day) {
            var wrap = document.createElement('div');
            wrap.className = 'schedule-day';

            var hdr = document.createElement('div');
            hdr.className = 'schedule-day-header';
            hdr.textContent = day.date;
            wrap.appendChild(hdr);

            (day.games || []).forEach(function (g) {
                var row = document.createElement('div');
                row.className = 'schedule-game';

                var matchup = document.createElement('span');
                matchup.className = 'schedule-matchup';
                matchup.textContent = g.away + ' @ ' + g.home;

                var stateEl = document.createElement('span');
                var st = g.state || '';
                var det = g.detailed || '';
                if (st === 'Live') {
                    stateEl.className = 'schedule-state live';
                    stateEl.textContent = det || 'LIVE';
                } else if (st === 'Final') {
                    stateEl.className = 'schedule-state final';
                    stateEl.textContent = 'Final';
                } else {
                    stateEl.className = 'schedule-state';
                    stateEl.textContent = '';
                }

                var timeEl = document.createElement('span');
                timeEl.className = 'schedule-time';
                timeEl.textContent = g.time;

                row.appendChild(matchup);
                row.appendChild(timeEl);
                if (stateEl.textContent) row.appendChild(stateEl);
                wrap.appendChild(row);
            });

            container.appendChild(wrap);
        });
    }

    function refreshSchedule() {
        api('/schedule').then(function (r) { return r.json(); }).then(function (d) {
            renderSchedule(d);
        }).catch(function () {});
    }

    refreshSchedule();
    setInterval(refreshSchedule, 60000);

    /* ── Test display ── */
    document.getElementById('btn-test-display').addEventListener('click', function () {
        api('/test_display', { method: 'POST' }).catch(function () {});
    });

    /* ── Test strobe ── */
    document.getElementById('btn-test-strobe').addEventListener('click', function () {
        var btn = this;
        var out = document.getElementById('strobe-test-out');
        btn.disabled = true;
        btn.textContent = 'Firing…';
        out.textContent = '';
        var teamId = configTeams.length > 0 ? configTeams[0].team_id : 0;
        api('/test_strobe', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ team_id: teamId }),
        }).then(function (r) { return r.json(); }).then(function (d) {
            var lines = [
                'Result: ' + d.message,
                'API available: ' + d.strobe_api_available,
                'Strobe enabled: ' + d.strobe_enabled,
                'Accent color:  ' + d.accent_color,
                'Mapped color:  ' + d.mapped_color,
                'Palette size:  ' + d.palette_colors + (d.palette_colors === 0 ? ' (full RGB)' : ' fixed colors'),
            ];
            out.textContent = lines.join('\n');
        }).catch(function (e) {
            out.textContent = 'Request failed: ' + e;
        }).finally(function () {
            btn.disabled = false;
            btn.textContent = 'Test Strobe';
        });
    });

})();
