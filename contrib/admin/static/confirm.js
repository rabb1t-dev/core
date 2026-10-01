// Confirmation prompts for destructive actions.
//
// These live in an external file rather than inline onsubmit/onclick handlers so the
// Content-Security-Policy can forbid inline script entirely.
//
// A form may carry data-confirm, and so may an individual submit button - needed where
// one form has several buttons with different formaction targets (restart vs stop).
document.addEventListener('submit', function (event) {
  var form = event.target;
  if (!form || form.tagName !== 'FORM') { return; }

  var submitter = event.submitter;
  var message = (submitter && submitter.getAttribute('data-confirm')) ||
                form.getAttribute('data-confirm');

  if (message && !window.confirm(message)) {
    event.preventDefault();
    event.stopPropagation();
  }
});
