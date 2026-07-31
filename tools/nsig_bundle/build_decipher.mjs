import { build } from 'esbuild';
await build({ entryPoints:['src/decipher_entry.ts'], bundle:true, outfile:'dist/nsig_decipher.min.js',
  format:'iife', target:['es2015'], platform:'neutral', legalComments:'none', minify:true });
console.log('built dist/nsig_decipher.min.js');
