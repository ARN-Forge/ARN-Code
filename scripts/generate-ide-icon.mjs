// Reproduce the CLI's block-character crab as font-independent SVG geometry.
import { writeFileSync } from 'node:fs';
const rows = ['▄▄          ▄▄', '▄▀██▄▀██▀▄██▀▄', '   ▀██████▀', '    ▄▀▀▀▀▄'];
const cells = rows.flatMap((row, y) => [...row].flatMap((glyph, x) => {
  if (glyph === ' ') return [];
  const top = 152 + y * 52 + (glyph === '▄' ? 26 : 0);
  return [`    <rect x="${74 + x * 26}" y="${top}" width="26" height="${glyph === '█' ? 52 : 26}"/>`];
}));
const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 512 512">
  <title>Arny, the ARN crab</title>
  <rect x="16" y="16" width="480" height="480" rx="96" fill="#101820"/>
  <g fill="#ff9820" shape-rendering="crispEdges">
${cells.join('\n')}
  </g>
</svg>
`;
writeFileSync(new URL('../ide/app-icon.svg', import.meta.url), svg);
