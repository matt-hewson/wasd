// WASD site: spoiler-tier demo + "send to your PC". Everything works without this file.
(function () {
  var demo = document.querySelector('[data-demo]');
  if (demo) {
    var blocks = demo.querySelectorAll('[data-tier]');
    var status = demo.querySelector('[data-demo-status]');
    var show = function (tier, announce) {
      blocks.forEach(function (b) { b.classList.toggle('on', b.getAttribute('data-tier') === tier); b.hidden = b.getAttribute('data-tier') !== tier; });
      if (announce && status) status.textContent = 'Showing ' + tier + ' tier';
    };
    demo.querySelectorAll('input[name="tier"]').forEach(function (r) {
      r.addEventListener('change', function () { if (r.checked) show(r.value, true); });
    });
    var checked = demo.querySelector('input[name="tier"]:checked');
    show(checked ? checked.value : 'Category', false);
  }

  var share = document.querySelector('[data-share]');
  if (share) {
    var msg = document.querySelector('[data-share-msg]');
    var url = location.origin + location.pathname;
    share.addEventListener('click', function () {
      if (navigator.share) {
        navigator.share({ title: 'WASD: a companion for Dark Souls III', url: url }).catch(function () {});
      } else if (navigator.clipboard) {
        navigator.clipboard.writeText(url).then(function () { if (msg) msg.textContent = 'Link copied. Paste it somewhere your PC can see it.'; });
      } else if (msg) {
        msg.textContent = url;
      }
    });
  }
})();
