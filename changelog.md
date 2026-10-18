# v0.2.8

- the package now ships `about.md`, `changelog.md` and `support.md`
- builds run on GitHub Actions and releases live on GitHub
- builds for macOS, android and ios now - the row-hover delete button stays windows-only
- readme and about page got a screenshot
- mod links point at this repo

# v0.2.7

- picking a nong now refreshes that row's name and artist right away, instead of waiting for a scroll

# v0.2.6

- fixed the name/artist placement for real - the labels were a full row too high (row space is centered on the row's middle, not bottom-left)
- name sits just above the row's middle, artist just below it - same feel as the original browser

# v0.2.5

- tighter text pass: name max size 0.55, text hugging the row's left edge

# v0.2.4

- long titles shrink to fit instead of getting cut short; "..." is only for truly absurd names now
- the search bar renders above the rows, so scrolling rows pass behind it

# v0.2.3

- the disc on each row now opens Jukebox's nong picker for that song

# v0.2.2

- live in-use state, recursive NONG lookup, scrolling/spacing polish
- filter buttons dropped - searching covers it

# v0.2.1

- fixed filter icons, a NONG pickup race, and scrolling is eased now

# v0.2.0

- first Geode build: fuzzy search across name + artist, preview, delete, select, settings
