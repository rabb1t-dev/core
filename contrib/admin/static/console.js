// Command reference: filter in the page and click a name into the input box.
(function () {
  var filter = document.getElementById('ref-filter');
  var consoleOnly = document.getElementById('ref-console');
  var counter = document.getElementById('ref-count');
  var empty = document.getElementById('ref-empty');
  var groups = Array.prototype.slice.call(document.querySelectorAll('.ref-group'));
  var input = document.getElementById('cmd');
  if (!filter || !groups.length) return;

  groups.forEach(function (g) {
    g.rows = Array.prototype.slice.call(g.querySelectorAll('.ref-row'));
  });

  function apply() {
    var q = filter.value.trim().toLowerCase();
    var needConsole = consoleOnly.checked;
    var shown = 0;
    var openAll = q.length > 1;

    groups.forEach(function (g) {
      var hit = 0;
      g.rows.forEach(function (r) {
        var ok = (!needConsole || r.dataset.console === '1') &&
                 (!q || r.dataset.hay.toLowerCase().indexOf(q) !== -1);
        r.hidden = !ok;
        if (ok) hit++;
      });
      g.hidden = hit === 0;
      g.querySelector('.count').textContent = hit;
      // Expanding on a search is the point of searching; collapsing again on clear
      // keeps the page from staying 800 rows tall afterwards.
      if (openAll && hit) g.open = true;
      else if (!q && !needConsole) g.open = false;
      shown += hit;
    });

    counter.textContent = shown + ' command' + (shown === 1 ? '' : 's');
    empty.hidden = shown !== 0;
  }

  filter.addEventListener('input', apply);
  consoleOnly.addEventListener('change', apply);

  document.addEventListener('click', function (e) {
    var btn = e.target.closest('.ref-row .linkish');
    if (!btn || !input) return;
    var row = btn.closest('.ref-row');
    input.value = row.dataset.cmd + (row.querySelector('.args').textContent.trim() ? ' ' : '');
    input.focus();
    input.setSelectionRange(input.value.length, input.value.length);
    window.scrollTo({ top: 0, behavior: 'smooth' });
  });
})();
