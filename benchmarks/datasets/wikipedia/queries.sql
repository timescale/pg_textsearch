-- Wikipedia Dataset - Query Benchmarks
-- Runs various query workloads against the indexed Wikipedia collection
-- Outputs structured timing data for historical tracking

\set ON_ERROR_STOP on
\timing on

\echo '=== Wikipedia Query Benchmarks ==='
\echo ''

-- Get dataset size
SELECT 'Dataset size: ' || COUNT(*) || ' articles' as info FROM wikipedia_articles;

-- Create benchmark queries table with token buckets (matching MS MARCO format)
-- Using representative Wikipedia queries across different token counts
\echo 'Creating benchmark queries...'
DROP TABLE IF EXISTS benchmark_queries;
CREATE TABLE benchmark_queries (
    query_id SERIAL,
    query_text TEXT,
    token_bucket INTEGER
);

-- Insert 100 queries per bucket (800 total) for comprehensive benchmarking
-- Bucket 1: 1 token
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'algorithm', 'history', 'science', 'computer', 'biology',
    'mathematics', 'physics', 'chemistry', 'geography', 'philosophy',
    'economics', 'psychology', 'medicine', 'engineering', 'astronomy',
    'geology', 'linguistics', 'anthropology', 'sociology', 'ecology',
    'democracy', 'evolution', 'electricity', 'architecture', 'literature',
    'music', 'art', 'religion', 'politics', 'technology',
    'agriculture', 'industry', 'commerce', 'military', 'education',
    'language', 'culture', 'society', 'nature', 'universe',
    'atom', 'molecule', 'cell', 'gene', 'protein',
    'bacteria', 'virus', 'plant', 'animal', 'human',
    'brain', 'heart', 'blood', 'bone', 'muscle',
    'water', 'air', 'earth', 'fire', 'metal',
    'energy', 'force', 'mass', 'velocity', 'temperature',
    'pressure', 'volume', 'density', 'gravity', 'magnetism',
    'radiation', 'wave', 'particle', 'quantum', 'relativity',
    'carbon', 'oxygen', 'hydrogen', 'nitrogen', 'iron',
    'gold', 'silver', 'copper', 'lead', 'zinc',
    'ocean', 'river', 'mountain', 'desert', 'forest',
    'island', 'continent', 'volcano', 'earthquake', 'climate',
    'weather', 'season', 'rain', 'snow', 'wind'
]), 1;

-- Bucket 2: 2 tokens
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'machine learning', 'world war', 'climate change', 'solar system', 'human body',
    'ancient history', 'modern art', 'nuclear physics', 'organic chemistry', 'cell biology',
    'quantum mechanics', 'natural selection', 'genetic engineering', 'artificial intelligence', 'computer science',
    'space exploration', 'ocean current', 'mountain range', 'river delta', 'tropical forest',
    'political science', 'economic theory', 'social psychology', 'cultural anthropology', 'medical research',
    'civil war', 'cold war', 'industrial revolution', 'french revolution', 'american revolution',
    'roman empire', 'british empire', 'ottoman empire', 'mongol empire', 'persian empire',
    'greek philosophy', 'eastern philosophy', 'religious history', 'art history', 'music theory',
    'literary criticism', 'film history', 'theater arts', 'dance history', 'architecture design',
    'urban planning', 'environmental science', 'marine biology', 'plant science', 'animal behavior',
    'human evolution', 'primate research', 'fossil record', 'geological time', 'plate tectonics',
    'volcanic activity', 'seismic waves', 'atmospheric science', 'weather patterns', 'ocean temperature',
    'global warming', 'carbon dioxide', 'renewable energy', 'nuclear energy', 'solar power',
    'wind energy', 'water cycle', 'nitrogen cycle', 'carbon cycle', 'food chain',
    'ecosystem dynamics', 'biodiversity loss', 'species extinction', 'habitat destruction', 'conservation biology',
    'population growth', 'demographic transition', 'migration patterns', 'urban growth', 'rural development',
    'agricultural practices', 'crop production', 'livestock farming', 'food security', 'water resources',
    'mineral resources', 'energy resources', 'forest resources', 'fishery resources', 'land use',
    'transportation systems', 'communication technology', 'information systems', 'network infrastructure', 'digital technology',
    'data science', 'cyber security', 'cloud computing', 'internet protocol', 'mobile technology'
]), 2;

-- Bucket 3: 3 tokens
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'world war two', 'machine learning algorithms', 'climate change effects', 'human immune system', 'solar system planets',
    'ancient greek philosophy', 'modern art movements', 'nuclear power plants', 'organic chemistry compounds', 'cell division process',
    'quantum computing applications', 'natural language processing', 'genetic modification techniques', 'artificial neural networks', 'computer network protocols',
    'space shuttle missions', 'deep ocean exploration', 'mountain formation processes', 'river flood control', 'tropical rainforest biodiversity',
    'political party systems', 'economic growth factors', 'social media impact', 'cultural heritage preservation', 'medical imaging technology',
    'american civil war', 'european cold war', 'industrial revolution effects', 'french revolution causes', 'russian revolution history',
    'roman empire decline', 'british colonial history', 'ottoman empire expansion', 'mongol invasion europe', 'persian empire culture',
    'greek mythology stories', 'eastern religious traditions', 'medieval church history', 'renaissance art masters', 'classical music composers',
    'shakespearean theater plays', 'hollywood film industry', 'modern dance pioneers', 'gothic architecture style', 'sustainable building design',
    'smart city planning', 'marine ecosystem health', 'plant photosynthesis process', 'animal migration routes', 'human brain function',
    'primate social behavior', 'dinosaur fossil discoveries', 'ice age periods', 'continental drift theory', 'volcanic eruption types',
    'earthquake prediction methods', 'hurricane formation process', 'drought impact agriculture', 'flood prevention measures', 'wildfire management strategies',
    'greenhouse gas emissions', 'ozone layer depletion', 'solar panel efficiency', 'wind turbine technology', 'hydroelectric power generation',
    'electric vehicle development', 'hydrogen fuel cells', 'battery storage systems', 'smart grid technology', 'energy efficiency standards',
    'endangered species protection', 'wildlife habitat restoration', 'marine protected areas', 'forest conservation efforts', 'wetland ecosystem services',
    'world population trends', 'aging population challenges', 'international migration flows', 'megacity development patterns', 'rural poverty reduction',
    'sustainable agriculture methods', 'precision farming technology', 'organic food production', 'global food distribution', 'clean water access',
    'rare earth minerals', 'offshore oil drilling', 'coal mining impacts', 'timber harvesting practices', 'sustainable fishing methods',
    'deep sea mining', 'arctic ice melting', 'ocean acidification effects', 'coral reef bleaching', 'mangrove forest destruction',
    'amazon rainforest fires', 'african savanna wildlife', 'australian outback ecosystem', 'siberian permafrost thawing', 'antarctic ice shelf'
]), 3;

-- Bucket 4: 4 tokens
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'second world war battles', 'deep learning neural networks', 'global climate change impacts', 'human cardiovascular system function', 'outer solar system exploration',
    'ancient greek city states', 'abstract expressionist art movement', 'nuclear fusion energy research', 'aromatic organic chemistry reactions', 'stem cell research applications',
    'quantum entanglement physics experiments', 'large language model training', 'crispr gene editing technology', 'convolutional neural network architectures', 'tcp ip protocol stack',
    'international space station missions', 'mariana trench deep exploration', 'himalayan mountain range formation', 'amazon river basin ecosystem', 'congo rainforest wildlife diversity',
    'parliamentary democracy political systems', 'keynesian economic growth theory', 'social network analysis methods', 'indigenous cultural preservation efforts', 'magnetic resonance imaging technology',
    'american civil war reconstruction', 'berlin wall cold war', 'steam engine industrial revolution', 'bastille storming french revolution', 'bolshevik october revolution russia',
    'fall of roman empire', 'east india company colonialism', 'siege of constantinople ottoman', 'genghis khan mongol conquests', 'cyrus the great persia',
    'olympian gods greek mythology', 'buddhist meditation eastern traditions', 'catholic church medieval europe', 'sistine chapel renaissance masterpiece', 'beethoven symphony classical music',
    'hamlet shakespeare tragedy play', 'golden age hollywood cinema', 'martha graham modern dance', 'notre dame gothic cathedral', 'leed certified building design',
    'singapore smart city initiative', 'coral reef marine ecosystem', 'c4 plant photosynthesis pathway', 'arctic tern migration route', 'prefrontal cortex brain function',
    'great ape social behavior', 'tyrannosaurus rex fossil discovery', 'pleistocene ice age mammals', 'wegener continental drift hypothesis', 'mount vesuvius eruption pompeii',
    'san andreas fault earthquakes', 'atlantic hurricane season patterns', 'sahel region drought impacts', 'mississippi river flood history', 'california wildfire prevention strategies',
    'methane greenhouse gas emissions', 'antarctic ozone hole recovery', 'perovskite solar cell efficiency', 'offshore wind farm development', 'three gorges dam hydroelectric',
    'tesla electric vehicle technology', 'toyota mirai hydrogen fuel', 'lithium ion battery advances', 'european smart grid rollout', 'passive house energy standards',
    'giant panda conservation success', 'yellowstone wolf reintroduction program', 'great barrier reef protection', 'amazon deforestation prevention efforts', 'chesapeake bay wetland restoration',
    'sub saharan africa population', 'japan aging society challenges', 'syrian refugee migration crisis', 'tokyo metropolitan area growth', 'appalachian poverty reduction programs',
    'vertical farming urban agriculture', 'john deere precision farming', 'usda organic certification standards', 'world food programme distribution', 'flint michigan water crisis',
    'democratic republic congo minerals', 'deepwater horizon oil spill', 'appalachian coal mining decline', 'canadian boreal forest logging', 'north sea fishing quotas',
    'mediterranean sea plastic pollution', 'indian monsoon climate patterns', 'sahara desert solar potential', 'amazon basin water resources', 'ganges river pollution levels',
    'california drought water management', 'great lakes ecosystem health', 'colorado river water rights', 'nile river dam construction', 'yangtze river flood control'
]), 4;

-- Bucket 5: 5 tokens
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'battle of stalingrad world war', 'transformer architecture deep learning models', 'paris agreement climate change targets', 'human heart blood circulation system', 'voyager probe outer solar system',
    'athenian democracy ancient greek politics', 'jackson pollock abstract expressionist paintings', 'iter tokamak nuclear fusion reactor', 'benzene ring aromatic compound chemistry', 'induced pluripotent stem cell research',
    'double slit quantum interference experiment', 'gpt transformer language model architecture', 'cas9 crispr gene editing mechanism', 'resnet deep convolutional neural network', 'border gateway protocol internet routing',
    'hubble space telescope deep field', 'challenger deep mariana trench dive', 'mount everest himalayan peak climbing', 'amazon river dolphin endangered species', 'bonobo great ape social structure',
    'westminster parliamentary democracy united kingdom', 'phillips curve inflation unemployment tradeoff', 'facebook social network growth analysis', 'maori cultural heritage new zealand', 'functional mri brain imaging technology',
    'freedmens bureau civil war reconstruction', 'checkpoint charlie berlin wall crossing', 'spinning jenny textile industrial revolution', 'tennis court oath french revolution', 'winter palace storming october revolution',
    'diocletian reforms late roman empire', 'robert clive east india company', 'mehmed conquest of constantinople fall', 'battle of kalka river mongols', 'achaemenid royal road persian empire',
    'twelve labors of hercules mythology', 'eightfold path buddhist enlightenment practice', 'great schism eastern western church', 'michelangelo david renaissance sculpture masterpiece', 'ninth symphony beethoven choral finale',
    'to be or not hamlet', 'citizen kane orson welles masterpiece', 'appalachian spring martha graham ballet', 'flying buttress notre dame cathedral', 'passive house institute energy standard',
    'autonomous vehicle smart city integration', 'great pacific garbage patch marine', 'rubisco enzyme c3 plant photosynthesis', 'bar tailed godwit record migration', 'broca area speech language processing',
    'jane goodall chimpanzee behavior research', 'sue tyrannosaurus rex field museum', 'woolly mammoth ice age extinction', 'alfred wegener pangaea supercontinent theory', 'pyroclastic flow mount vesuvius eruption',
    'usgs earthquake early warning system', 'saffir simpson hurricane intensity scale', 'dust bowl great plains drought', 'levee failure hurricane katrina flooding', 'cal fire wildfire suppression operations',
    'permafrost thaw methane release arctic', 'montreal protocol ozone layer protection', 'perovskite tandem solar cell record', 'hornsea offshore wind farm uk', 'itaipu dam hydroelectric power output',
    'tesla model s electric range', 'hyundai nexo hydrogen fuel efficiency', 'solid state battery energy density', 'nord pool smart grid trading', 'net zero energy building design',
    'chengdu giant panda breeding center', 'yellowstone grizzly bear population recovery', 'great barrier reef bleaching events', 'brazilian amazon soy moratorium deforestation', 'chesapeake bay oyster reef restoration',
    'mount everest climbing expedition routes', 'mariana trench deep sea creatures', 'amazon river basin indigenous peoples', 'sahara desert ancient trade routes', 'antarctic research station operations',
    'mediterranean diet health benefits study', 'japanese bullet train technology development', 'german autobahn speed limit debate', 'swiss alps tunnel construction projects', 'panama canal expansion modernization project',
    'suez canal maritime shipping traffic', 'gibraltar strait underwater tunnel proposal', 'bering strait land bridge theory', 'english channel tunnel rail service', 'golden gate bridge construction history',
    'brooklyn bridge engineering innovation design', 'sydney harbour bridge construction workers', 'tower bridge london opening mechanism', 'london underground metro system history', 'new york subway expansion plans',
    'paris metro art nouveau stations', 'moscow metro palatial underground architecture', 'tokyo metro busiest railway system', 'beijing subway rapid expansion growth', 'shanghai maglev train airport connection'
]), 5;

-- Bucket 6: 6 tokens
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'operation barbarossa german invasion soviet union', 'attention mechanism transformer neural network architecture', 'intergovernmental panel climate change ipcc reports', 'sinoatrial node heart electrical conduction system', 'new horizons pluto kuiper belt flyby',
    'pericles funeral oration athenian democracy speech', 'number one jackson pollock drip painting', 'national ignition facility laser fusion experiment', 'nucleophilic aromatic substitution organic chemistry mechanism', 'shinya yamanaka induced pluripotent stem cells',
    'delayed choice quantum eraser experiment results', 'bidirectional encoder representations transformers bert model', 'jennifer doudna emmanuelle charpentier crispr discovery', 'imagenet large scale visual recognition challenge', 'autonomous system number bgp internet routing',
    'james webb space telescope first images', 'don walsh jacques piccard trieste bathyscaphe', 'tenzing norgay edmund hillary everest summit', 'amazon river basin deforestation satellite monitoring', 'bonobos matriarchal society female social dominance',
    'house of commons parliamentary procedure rules', 'natural rate of unemployment nairu concept', 'cambridge analytica facebook data privacy scandal', 'te papa tongarewa maori taonga collection', 'diffusion tensor imaging white matter tracts',
    'fourteenth amendment equal protection clause reconstruction', 'tear down this wall reagan speech', 'richard arkwright water frame spinning machine', 'declaration rights of man citizen france', 'vladimir lenin april theses bolshevik revolution',
    'constantine edict of milan christian legalization', 'battle of plassey british india conquest', 'hagia sophia ottoman mosque christian cathedral', 'battle of liegnitz mongol polish encounter', 'cyrus cylinder human rights ancient declaration',
    'perseus slaying medusa greek hero mythology', 'four noble truths buddhist suffering doctrine', 'papal infallibility first vatican council doctrine', 'sistine chapel ceiling creation of adam', 'ode to joy ninth symphony finale',
    'yorick skull gravedigger scene hamlet soliloquy', 'rosebud sled ending citizen kane mystery', 'night journey martha graham cave heart', 'chartres cathedral labyrinth medieval pilgrimage symbol', 'passivhaus institut darmstadt germany certification standard',
    'waymo autonomous vehicle phoenix arizona deployment', 'microplastics great pacific garbage patch accumulation', 'photorespiration rubisco oxygenase c3 plant inefficiency', 'e7 bar tailed godwit alaska migration', 'wernicke area language comprehension temporal lobe',
    'gombe stream national park chimpanzee research', 'sue tyrannosaurus rex chicago field museum', 'la brea tar pits ice age', 'south atlantic seafloor spreading mid ocean', 'herculaneum preservation mount vesuvius pyroclastic surge',
    'shakealert earthquake early warning los angeles', 'hurricane maria puerto rico category five', 'grapes of wrath dust bowl migration', 'hurricane katrina lower ninth ward flooding', 'paradise california camp fire town destruction',
    'siberian permafrost crater methane explosion events', 'kigali amendment hfc phase down protocol', 'oxford pv perovskite silicon tandem efficiency', 'dogger bank offshore wind farm north', 'three gorges dam yangtze river capacity',
    'tesla supercharger network electric vehicle charging', 'hyundai nexo 380 mile hydrogen range', 'quantumscape solid state lithium metal battery', 'entsoe european smart grid frequency regulation', 'bullitt center seattle net zero building',
    'wolong giant panda nature reserve china', 'northern rocky mountain wolf recovery program', 'coral bleaching great barrier reef australia', 'amazon soy moratorium cattle ranching deforestation', 'harris creek oyster restoration chesapeake bay',
    'mount kilimanjaro glacier retreat climate change', 'lake baikal freshwater ecosystem biodiversity russia', 'galapagos islands darwin finch evolution study', 'madagascar lemur species endangered habitat loss', 'borneo orangutan rainforest conservation efforts indonesia',
    'serengeti wildebeest migration annual cycle tanzania', 'everglades wetland restoration project florida usa', 'dead sea shrinking water level jordan', 'caspian sea sturgeon caviar fishing industry', 'aral sea environmental disaster soviet irrigation',
    'venice flood barrier mose project construction', 'netherlands sea level rise delta works', 'new orleans hurricane levee protection system', 'tokyo bay land reclamation expansion project', 'hong kong airport artificial island construction',
    'dubai palm islands artificial archipelago development', 'singapore marina bay land reclamation project', 'shanghai pudong skyline rapid urban development', 'sydney opera house architectural design competition', 'bilbao guggenheim museum urban regeneration catalyst',
    'louvre museum paris glass pyramid renovation', 'british museum london elgin marbles controversy', 'smithsonian institution washington dc national mall', 'metropolitan museum art new york city', 'hermitage museum saint petersburg winter palace'
]), 6;

-- Bucket 7: 7 tokens
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'operation barbarossa largest military invasion in history', 'multi head self attention transformer encoder decoder', 'ipcc sixth assessment report climate change impacts', 'purkinje fibers heart ventricular electrical conduction system', 'voyager golden record sounds of earth message',
    'delian league athenian empire tribute collection system', 'autumn rhythm number 30 jackson pollock moma', 'lawrence livermore national ignition facility fusion breakthrough', 'electrophilic aromatic substitution friedel crafts alkylation mechanism', 'takahashi shinya yamanaka nobel prize stem cells',
    'which way delayed choice quantum eraser interference', 'google bert bidirectional transformer pretraining language model', 'crispr cas9 double strand break dna repair', 'alexnet imagenet classification deep convolutional neural network', 'border gateway protocol path vector routing algorithm',
    'jwst deep field first galaxies early universe', 'mariana trench hadal zone challenger deep exploration', 'hillary step mount everest summit death zone', 'modis satellite amazon deforestation monitoring terra aqua', 'bonobo make love not war conflict resolution',
    'speaker of the house parliamentary chamber rules', 'friedman phelps natural rate hypothesis inflation expectations', 'cambridge analytica facebook fifty million user breach', 'te papa tongarewa museum wellington new zealand', 'dti tractography neural pathway white matter imaging',
    'brown v board of education fourteenth amendment', 'mr gorbachev tear down this wall berlin', 'spinning mule samuel crompton cotton textile invention', 'rights of man thomas paine french revolution', 'sealed train journey lenin switzerland russia revolution',
    'council of nicaea constantine christian church doctrine', 'black hole of calcutta east india company', 'theodora justinian hagia sophia byzantine mosaic art', 'subutai mongol general european campaign battle tactics', 'cyrus the great cylinder british museum babylon',
    'athena birth from head of zeus mythology', 'middle way buddha enlightenment bodhi tree meditation', 'pope pius ix first vatican council infallibility', 'god touching adam sistine chapel ceiling fresco', 'schiller ode to joy beethoven ninth symphony',
    'alas poor yorick hamlet gravedigger skull scene', 'citizen kane rosebud sled childhood snow globe', 'martha graham cave of the heart medea', 'amiens cathedral gothic architecture flying buttress height', 'zero carbon passivhaus standard blower door airtightness',
    'waymo one robotaxi service phoenix metro area', 'five gyres institute microplastic pollution ocean research', 'c4 cam photosynthesis kranz anatomy bundle sheath', 'e7 alaska new zealand bar tailed godwit', 'broca wernicke arcuate fasciculus language processing circuit',
    'goodall gombe chimpanzee tool use termite fishing', 'sue tyrannosaurus rex skeleton chicago natural history', 'rancho la brea tar pits pleistocene fossils', 'mid atlantic ridge seafloor spreading magnetic striping', 'herculaneum scrolls villa papyri vesuvius carbonization preservation',
    'shakealert myshake earthquake early warning system california', 'hurricane maria puerto rico power grid total collapse', 'dust bowl migration grapes of wrath steinbeck', 'lower ninth ward levee failure hurricane katrina', 'camp fire paradise california deadliest wildfire history',
    'batagaika crater siberian permafrost thermokarst methane release', 'montreal protocol kigali amendment hfc refrigerant phasedown', 'perovskite silicon tandem solar cell 29 percent', 'dogger bank wind farm north sea capacity', 'three gorges dam largest hydroelectric power station',
    'tesla model 3 standard range plus efficiency', 'toyota mirai hydrogen fuel cell electric vehicle', 'quantumscape solid state battery ev range improvement', 'european network transmission system operators smart grid', 'bullitt center seattle living building challenge certified',
    'mount everest base camp altitude sickness prevention', 'pacific crest trail thru hike completion rate', 'appalachian trail longest hiking path eastern united', 'great wall china tourist visitor number statistics', 'machu picchu inca citadel archaeological site peru',
    'angkor wat temple complex cambodia world heritage', 'petra ancient city jordan rose red sandstone', 'colosseum rome gladiator arena seating capacity history', 'parthenon athens acropolis greek temple marble columns', 'stonehenge prehistoric monument wiltshire england summer solstice',
    'pyramids giza egypt pharaoh tomb construction mystery', 'taj mahal india mughal architecture white marble', 'eiffel tower paris iron lattice gustave construction', 'statue liberty new york copper neoclassical sculpture', 'big ben london clock tower westminster palace',
    'leaning tower pisa italy bell tower inclination', 'christ redeemer rio janeiro brazil art deco', 'mount rushmore presidential sculpture black hills south', 'golden gate bridge san francisco suspension span', 'brooklyn bridge new york east river gothic',
    'sydney harbour bridge australia steel arch design', 'tower bridge london victorian gothic bascule drawbridge', 'burj khalifa dubai tallest building world skyscraper', 'empire state building new york art deco', 'petronas towers kuala lumpur twin skyscraper malaysia',
    'cn tower toronto canada observation deck height', 'sears tower chicago willis tower rename controversy', 'one world trade center new york memorial', 'taipei 101 tower taiwan tuned mass damper', 'shanghai tower china twisted form sustainable design'
]), 7;

-- Bucket 8: 8+ tokens
INSERT INTO benchmark_queries (query_text, token_bucket)
SELECT unnest(ARRAY[
    'operation barbarossa june 1941 german invasion of soviet union', 'scaled dot product attention multi head self attention mechanism', 'ipcc ar6 synthesis report limiting global warming 1.5 degrees', 'sinoatrial node atrioventricular node bundle of his conduction', 'voyager 1 golden record sounds of earth carl sagan',
    'delian league treasury moved from delos to athens tribute', 'autumn rhythm number 30 1950 jackson pollock moma collection', 'national ignition facility lawrence livermore 3.15 megajoule fusion yield', 'electrophilic aromatic substitution mechanism benzene friedel crafts acylation reaction', 'yamanaka factors oct4 sox2 klf4 myc induced pluripotent stem',
    'wheeler delayed choice quantum eraser experiment wave particle duality', 'bert masked language model next sentence prediction pretraining task', 'crispr cas9 guide rna pam sequence double strand break', 'alexnet krizhevsky sutskever hinton imagenet 2012 deep learning breakthrough', 'bgp path vector protocol autonomous system number prefix announcement',
    'james webb space telescope first deep field image release', 'challenger deep mariana trench 10994 meters deepest ocean point', 'tenzing norgay edmund hillary may 29 1953 everest summit', 'amazon basin deforestation monitoring modis landsat satellite time series', 'bonobo pan paniscus great ape female dominance sexual behavior',
    'speaker of the house of commons parliamentary procedure mace', 'phillips curve friedman phelps expectations augmented natural rate hypothesis', 'cambridge analytica alexander nix facebook user data trump campaign', 'te papa tongarewa national museum new zealand wellington waterfront', 'diffusion tensor imaging fractional anisotropy white matter integrity measure',
    'brown v board of education 1954 separate but equal overturned', 'reagan tear down this wall brandenburg gate berlin speech', 'samuel crompton spinning mule 1779 cotton textile industrial revolution', 'declaration of the rights of man and citizen 1789', 'vladimir lenin sealed train journey petrograd finland station april',
    'council of nicaea 325 ad nicene creed constantine christianity', 'battle of plassey 1757 robert clive east india company bengal', 'hagia sophia 537 justinian theodora byzantine mosaic orthodox mosque', 'battle of mohi 1241 mongol invasion hungary batu khan europe', 'cyrus cylinder 539 bc persian empire human rights declaration babylon',
    'athena goddess of wisdom born from head of zeus', 'siddhartha gautama buddha four noble truths eightfold path enlightenment', 'first vatican council 1870 pope pius ix papal infallibility dogma', 'michelangelo sistine chapel ceiling creation of adam god fresco', 'ludwig van beethoven ninth symphony ode to joy choral finale',
    'to be or not to be hamlet act 3', 'citizen kane 1941 orson welles rosebud sled childhood mystery', 'martha graham cave of the heart medea greek tragedy dance', 'chartres cathedral gothic architecture labyrinth pilgrimage rose window stained glass', 'passivhaus standard airtightness heat recovery ventilation thermal bridge free construction',
    'waymo autonomous vehicle level 4 self driving robotaxi phoenix', 'great pacific garbage patch north pacific subtropical convergence zone microplastics', 'c4 photosynthesis kranz anatomy bundle sheath mesophyll cell co2 concentration', 'bar tailed godwit e7 alaska new zealand 11000 km nonstop', 'broca wernicke model arcuate fasciculus language network dual stream processing',
    'jane goodall gombe stream chimpanzee tool use termite fishing observation', 'sue tyrannosaurus rex field museum chicago largest most complete skeleton', 'rancho la brea tar pits los angeles pleistocene ice age', 'mid atlantic ridge seafloor spreading paleomagnetism vine matthews morley hypothesis', 'herculaneum papyrus scrolls villa of papyri vesuvius 79 ad carbonized',
    'shakealert earthquake early warning system california usgs seismic network alert', 'hurricane maria 2017 puerto rico category 5 power grid collapse', 'dust bowl 1930s great plains drought soil erosion okies migration', 'hurricane katrina 2005 new orleans levee failure lower ninth ward', 'camp fire 2018 paradise california deadliest most destructive wildfire history',
    'batagaika megaslump siberia permafrost thaw thermokarst crater methane emissions climate', 'montreal protocol ozone layer cfcs hfcs kigali amendment phasedown schedule', 'oxford pv perovskite silicon tandem solar cell 29.5 percent efficiency', 'dogger bank offshore wind farm north sea 3.6 gw capacity', 'three gorges dam yangtze river 22500 mw largest hydroelectric station',
    'tesla model 3 long range dual motor all wheel drive', 'toyota mirai second generation hydrogen fuel cell 402 mile range', 'quantumscape solid state lithium metal anode battery energy density range', 'european network transmission system operators entsoe frequency containment reserve balancing', 'bullitt center seattle six story office living building challenge net zero',
    'wolong national nature reserve giant panda conservation breeding chengdu sichuan', 'yellowstone to yukon conservation initiative grizzly bear wolf corridor connectivity', 'great barrier reef marine park authority coral bleaching mass mortality', 'amazon soy moratorium 2006 cattle ranching deforestation cerrado biome expansion', 'harris creek oyster reef restoration project chesapeake bay largest maryland',
    'mount everest summit expedition 1953 tenzing norgay edmund hillary first ascent', 'apollo 11 moon landing 1969 neil armstrong buzz aldrin lunar module', 'wright brothers first powered flight 1903 kitty hawk north carolina flyer', 'charles lindbergh transatlantic solo flight 1927 spirit of st louis paris', 'amelia earhart circumnavigation attempt 1937 disappearance pacific ocean lockheed electra',
    'titanic sinking 1912 north atlantic iceberg collision lifeboats passengers crew', 'hindenburg disaster 1937 lakehurst new jersey hydrogen airship fire explosion', 'challenger space shuttle disaster 1986 o ring failure solid rocket booster', 'columbia space shuttle disaster 2003 foam insulation thermal protection system damage', 'chernobyl nuclear disaster 1986 reactor explosion radiation contamination exclusion zone',
    'fukushima daiichi nuclear disaster 2011 tsunami reactor meltdown evacuation zone', 'deepwater horizon oil spill 2010 gulf mexico blowout environmental damage', 'exxon valdez oil spill 1989 prince william sound alaska tanker grounding', 'bhopal gas tragedy 1984 union carbide methyl isocyanate leak india', 'great smog london 1952 air pollution coal burning respiratory deaths',
    'spanish flu pandemic 1918 h1n1 influenza virus global death toll', 'black death bubonic plague 1347 europe yersinia pestis mortality rate', 'covid 19 pandemic 2020 sars cov 2 coronavirus global lockdown measures', 'world war i 1914 1918 trench warfare western front armistice', 'world war ii 1939 1945 nazi germany allied powers axis powers',
    'vietnam war 1955 1975 american military involvement viet cong guerrilla', 'korean war 1950 1953 armistice 38th parallel dmz north south', 'gulf war 1990 1991 operation desert storm iraq kuwait invasion', 'american civil war 1861 1865 union confederate slavery abolition lincoln', 'french revolution 1789 1799 reign of terror napoleon rise power'
]), 8;

SELECT 'Loaded ' || COUNT(*) || ' benchmark queries' as status FROM benchmark_queries;
SELECT token_bucket, COUNT(*) as count
FROM benchmark_queries GROUP BY token_bucket ORDER BY token_bucket;

-- Warm up: run queries from each bucket to ensure index is cached
\echo ''
\echo 'Warming up index...'
DO $$
DECLARE
    q record;
BEGIN
    FOR q IN SELECT query_text FROM benchmark_queries ORDER BY random() LIMIT 50 LOOP
        EXECUTE 'SELECT article_id FROM wikipedia_articles
                 ORDER BY content <@> to_bm25query($1, ''wikipedia_bm25_idx'')
                 LIMIT 10' USING q.query_text;
    END LOOP;
END;
$$;

-- ============================================================
-- Benchmark 1: Query Latency by Token Count
-- ============================================================
\echo ''
\echo '=== Benchmark 1: Query Latency by Token Count ==='
\echo 'Running 100 queries per token bucket, reporting p50/p95/p99'
\echo ''

-- Temp table to capture per-bucket results for weighted average
CREATE TEMP TABLE bucket_results (
    bucket int,
    p50_ms numeric, p95_ms numeric, p99_ms numeric,
    avg_ms numeric, num_queries int, total_results bigint
);

-- Function to benchmark a bucket and return percentiles
CREATE OR REPLACE FUNCTION benchmark_bucket(bucket int)
RETURNS TABLE(p50_ms numeric, p95_ms numeric, p99_ms numeric, avg_ms numeric,
              num_queries int, total_results bigint) AS $$
DECLARE
    q record;
    start_ts timestamp;
    end_ts timestamp;
    times numeric[];
    sorted_times numeric[];
    n int;
    result_count bigint;
    results_sum bigint := 0;
BEGIN
    times := ARRAY[]::numeric[];

    FOR q IN SELECT query_text FROM benchmark_queries
             WHERE token_bucket = bucket ORDER BY query_id LOOP
        start_ts := clock_timestamp();
        EXECUTE 'SELECT COUNT(*) FROM (SELECT article_id FROM wikipedia_articles
                 ORDER BY content <@> to_bm25query($1, ''wikipedia_bm25_idx'')
                 LIMIT 10) t' INTO result_count USING q.query_text;
        end_ts := clock_timestamp();
        times := array_append(times,
                              EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000);
        results_sum := results_sum + result_count;
    END LOOP;

    n := array_length(times, 1);
    SELECT array_agg(t ORDER BY t) INTO sorted_times FROM unnest(times) t;

    p50_ms := sorted_times[(n + 1) / 2];
    p95_ms := sorted_times[(n * 95 + 99) / 100];
    p99_ms := sorted_times[(n * 99 + 99) / 100];
    avg_ms := (SELECT AVG(t) FROM unnest(times) t);
    num_queries := n;
    total_results := results_sum;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

-- Run benchmarks for each token bucket (results saved for weighted avg)
\echo 'Token bucket 1 (1 search token):'
INSERT INTO bucket_results SELECT 1, * FROM benchmark_bucket(1);
SELECT 'LATENCY_BUCKET_1: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 1;

\echo ''
\echo 'Token bucket 2 (2 search tokens):'
INSERT INTO bucket_results SELECT 2, * FROM benchmark_bucket(2);
SELECT 'LATENCY_BUCKET_2: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 2;

\echo ''
\echo 'Token bucket 3 (3 search tokens):'
INSERT INTO bucket_results SELECT 3, * FROM benchmark_bucket(3);
SELECT 'LATENCY_BUCKET_3: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 3;

\echo ''
\echo 'Token bucket 4 (4 search tokens):'
INSERT INTO bucket_results SELECT 4, * FROM benchmark_bucket(4);
SELECT 'LATENCY_BUCKET_4: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 4;

\echo ''
\echo 'Token bucket 5 (5 search tokens):'
INSERT INTO bucket_results SELECT 5, * FROM benchmark_bucket(5);
SELECT 'LATENCY_BUCKET_5: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 5;

\echo ''
\echo 'Token bucket 6 (6 search tokens):'
INSERT INTO bucket_results SELECT 6, * FROM benchmark_bucket(6);
SELECT 'LATENCY_BUCKET_6: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 6;

\echo ''
\echo 'Token bucket 7 (7 search tokens):'
INSERT INTO bucket_results SELECT 7, * FROM benchmark_bucket(7);
SELECT 'LATENCY_BUCKET_7: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 7;

\echo ''
\echo 'Token bucket 8 (8+ search tokens):'
INSERT INTO bucket_results SELECT 8, * FROM benchmark_bucket(8);
SELECT 'LATENCY_BUCKET_8: p50=' || round(p50_ms, 2) || 'ms p95=' ||
       round(p95_ms, 2) || 'ms p99=' || round(p99_ms, 2) || 'ms avg=' ||
       round(avg_ms, 2) || 'ms (n=' || num_queries || ', results=' ||
       total_results || ')' as result
FROM bucket_results WHERE bucket = 8;

DROP FUNCTION benchmark_bucket;

-- ============================================================
-- Weighted-Average Query Latency (MS-MARCO v1 distribution)
-- ============================================================
-- Weights from observed lexeme distribution in 1,010,905 MS-MARCO
-- v1 queries after to_tsvector: 1=3.5%, 2=16.3%, 3=30.2%,
-- 4=26.1%, 5=14.2%, 6=5.9%, 7=2.2%, 8+=1.5%
\echo ''
\echo '=== Weighted-Average Query Latency ==='
SELECT 'WEIGHTED_LATENCY_RESULT: weighted_p50='
    || round(SUM(b.p50_ms * w.weight) / SUM(w.weight), 2)
    || 'ms weighted_avg='
    || round(SUM(b.avg_ms * w.weight) / SUM(w.weight), 2)
    || 'ms' as result
FROM bucket_results b
JOIN (VALUES
    (1, 35638), (2, 165033), (3, 304887), (4, 264177),
    (5, 143765), (6, 59558), (7, 22595), (8, 15252)
) AS w(bucket, weight) ON b.bucket = w.bucket;

DROP TABLE bucket_results;

-- ============================================================
-- Benchmark 2: Query Throughput (800 queries, 3 iterations)
-- ============================================================
\echo ''
\echo '=== Benchmark 2: Query Throughput (800 queries, 3 iterations) ==='
\echo 'Running all 800 benchmark queries with warmup'

-- Helper function for throughput benchmark
CREATE OR REPLACE FUNCTION benchmark_throughput(iterations int DEFAULT 3)
RETURNS TABLE(median_ms numeric, min_ms numeric, max_ms numeric,
              queries_run int) AS $$
DECLARE
    q record;
    i int;
    start_ts timestamp;
    end_ts timestamp;
    times numeric[];
    sorted_times numeric[];
    query_count int;
BEGIN
    SELECT COUNT(*) INTO query_count FROM benchmark_queries;

    -- Warmup: run all queries once
    FOR q IN SELECT query_text FROM benchmark_queries ORDER BY query_id LOOP
        EXECUTE 'SELECT article_id FROM wikipedia_articles
                 ORDER BY content <@> to_bm25query($1, ''wikipedia_bm25_idx'')
                 LIMIT 10' USING q.query_text;
    END LOOP;

    -- Timed iterations
    times := ARRAY[]::numeric[];
    FOR i IN 1..iterations LOOP
        start_ts := clock_timestamp();
        FOR q IN SELECT query_text FROM benchmark_queries ORDER BY query_id LOOP
            EXECUTE 'SELECT article_id FROM wikipedia_articles
                     ORDER BY content <@> to_bm25query($1, ''wikipedia_bm25_idx'')
                     LIMIT 10' USING q.query_text;
        END LOOP;
        end_ts := clock_timestamp();
        times := array_append(times,
                              EXTRACT(EPOCH FROM (end_ts - start_ts)) * 1000);
    END LOOP;

    SELECT array_agg(t ORDER BY t) INTO sorted_times FROM unnest(times) t;
    median_ms := sorted_times[(iterations + 1) / 2];
    min_ms := sorted_times[1];
    max_ms := sorted_times[iterations];
    queries_run := query_count;
    RETURN NEXT;
END;
$$ LANGUAGE plpgsql;

SELECT 'Execution Time: ' || round(median_ms / queries_run, 3) ||
       ' ms (min=' || round(min_ms / queries_run, 3) ||
       ', max=' || round(max_ms / queries_run, 3) || ')' as result,
       'THROUGHPUT_RESULT: ' || queries_run || ' queries in ' ||
       round(median_ms, 2) || ' ms (avg ' ||
       round(median_ms / queries_run, 2) || ' ms/query)' as summary
FROM benchmark_throughput();

DROP FUNCTION benchmark_throughput;

-- ============================================================
-- Benchmark 3: Index Statistics
-- ============================================================
\echo ''
\echo '=== Benchmark 3: Index Statistics ==='

SELECT
    'wikipedia_bm25_idx' as index_name,
    pg_size_pretty(pg_relation_size('wikipedia_bm25_idx')) as index_size,
    pg_size_pretty(pg_relation_size('wikipedia_articles')) as table_size,
    (SELECT COUNT(*) FROM wikipedia_articles) as num_documents;

-- Cleanup
DROP TABLE benchmark_queries;

\echo ''
\echo '=== Wikipedia Query Benchmarks Complete ==='
