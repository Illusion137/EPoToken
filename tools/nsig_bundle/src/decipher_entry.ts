// Minimal decipher-only bundle. Extraction is done natively in C++; this only
// evaluates the extracted decipher script and transforms `n`.
//   __epo_setupNsig(output) -> stash exportedVars on globalThis
//   __epo_decipherN(n)      -> deciphered n

function nsigProcessorBody(): string {
  return `
  var exportedVars = globalThis.__epo_vars;
  var mockStreamingURL = "https://ytjs.googlevideo.com/videoplayback?expire=1234567890&" + "n=" + encodeURIComponent(n);
  var urlCtorFunction = exportedVars.nsigFunction || (function(){ throw new Error('No n/sig decipher function extracted'); });
  var urlCtor = urlCtorFunction(mockStreamingURL, "", "");
  var proto = Object.getPrototypeOf(urlCtor);
  var properties = Object.getOwnPropertyNames(proto);
  var methodBlacklist = ['constructor', 'clone', 'set', 'get'];
  for (var i = 0; i < properties.length; i++) {
    var prop = properties[i];
    if (methodBlacklist.indexOf(prop) !== -1) continue;
    if (typeof urlCtor[prop] === 'function') urlCtor[prop]();
  }
  var nResult = urlCtor.get('n');
  return nResult ? decodeURIComponent(nResult) : "";`;
}

const g: any = (typeof globalThis !== 'undefined') ? globalThis : (function(){ return this; })();

g.__epo_setupNsig = function(output: string): boolean {
  const fn = new Function(output + '\nglobalThis.__epo_vars = exportedVars;\nreturn true;');
  return !!fn();
};

g.__epo_decipherN = function(n: string): string {
  const fn = new Function('n', nsigProcessorBody());
  return fn(n);
};
