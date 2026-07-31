import { build } from 'esbuild';
await build({
  entryPoints: ['src/entry.ts'], bundle: true, outfile: 'dist/nsig_extractor.min.js',
  format: 'iife', target: ['es2015'], platform: 'neutral', legalComments: 'none', minify: true
});
console.log('built dist/nsig_extractor.min.js');
