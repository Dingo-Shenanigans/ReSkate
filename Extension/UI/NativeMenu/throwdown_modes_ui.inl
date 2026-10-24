// Included inside throwdown_menu's private namespace: the Throwdowns page's logo grid (SmokeyDawg58's)
// with the stock throwdowns and every plugin mode, and the native flag cursor. All views are native
// Rime models, with the same checked ownership/publication helpers as the rest of the page.
void flag_cursor_height(const Context& c) {
    // Borrow the native RT movement ceiling only while our flag picker asks
    // for it. Ground validation and the other Throwdown pickers remain native.
    struct Lease {Address manager{};Value root{};NativeFlagCursorHeight height;};
    static Lease lease;
    const auto limit=[&](Value root) {
        require(root.handle && c.type_of(root.handle)==root.type,"Flag cursor model expired.");
        const auto settings=c.field(root,0xcb05963b);
        require(read<std::uint32_t>(read<Address>(settings.type))==0xdd5128d0 && size(settings.type)==112,
                "Flag cursor settings schema differs.");
        const auto value=c.field(settings,0xf08755ba);
        require(kind(value.type)==19 && size(value.type)==sizeof(float),"Flag cursor height type differs.");
        return value;
    };
    try {
        if(lease.manager!=c.manager)lease={}; // Never touch a retired model manager.
        const float wanted=flag_plumbing::cursor_height();
        const bool active=wanted>0.f;
        if(!active && !lease.root.handle)return;
        const auto roots=active?c.roots({3020238187U}):std::vector<Root>{};
        const Value next=roots.size()==1?roots.front().model:Value{};
        if(lease.root.handle && (!active || next.handle!=lease.root.handle || next.type!=lease.root.type)) {
            if(c.type_of(lease.root.handle)==lease.root.type) {
                const auto value=limit(lease.root);
                if(const auto original=lease.height.restore(read<float>(c.address(value))))c.set(value,*original);
            }
            lease={};
        }
        if(!active || !next.handle)return;
        const auto value=limit(next);
        lease.manager=c.manager;lease.root=next;
        if(const auto raised=lease.height.apply(read<float>(c.address(value)),wanted))c.set(value,*raised);
    } catch(const std::exception& error) {
        // An optional cursor override must never retire the challenge HUD.
        static bool reported{};
        if(!std::exchange(reported,true))logging::log(logging::Level::warning,logging::Channel::ui,
            "Flag movement settings unavailable: {}",error.what());
    }
}
// A mode's art: a texture or image asset path (a plugin's mod adds it), or a bare record name in
// Throwdown_ModeSelect_ContentResources, whose category record retains its loaded image.
void mode_art(const Context& c,std::span<const Value> fields,const std::string& logo) {
    if(logo.empty())return;
    try {
        if(logo.find('/')!=std::string::npos) {
            Address image{};
            try {image=asset(c,logo.c_str());} catch(const std::exception&) {}
            if(image && (read<Address>(image+8)==c.base+build::native_menu::texture_asset_type ||
                         read<Address>(image+8)==c.base+build::native_menu::image_asset_type)) {
                for(const auto field:fields)if(read<Address>(c.address(field))!=image)c.set(field,image);
                return;
            }
            // Texture registration is lazy: the renderer's table knows it after its first draw.
            require(publish_texture(c,fields,logo),"Throwdown art has not registered yet.");
            return;
        }
        const auto name=std::string("Throwdown_ModeSelect_ContentResources/")+logo;
        const auto record=asset(c,name.c_str());
        const auto type=read<Address>(record+0x18),data=read<Address>(record+0x20);
        require(read<Address>(record+8)==c.base+build::native_menu::asset_record_type && type && data &&
                read<std::uint32_t>(read<Address>(type))==category_schema.hash,"Throwdown logo record schema differs.");
        const auto image=read<Address>(data+c.member(type,0x189084da).offset);
        require(image && (read<Address>(image+8)==c.base+build::native_menu::texture_asset_type ||
                         read<Address>(image+8)==c.base+build::native_menu::image_asset_type),"Throwdown logo record image is not loaded.");
        for(const auto field:fields)if(read<Address>(c.address(field))!=image)c.set(field,image);
    }
    catch(const std::exception& error) {
        static std::set<std::string> reported;
        if(reported.insert(logo).second)logging::log(logging::Level::warning,logging::Channel::ui,
            "Throwdown optional artwork {} is unavailable; retaining native fallback: {}",logo,error.what());
    }
}
// Plugin modes in grid order: by their order, then id.
std::vector<const throwdowns::Definition*> plugin_definitions() {
    std::vector<const throwdowns::Definition*> result;
    for(const auto& d:throwdowns::registry().definitions())if(d.adapter==throwdowns::Adapter::plugin)result.push_back(&d);
    std::stable_sort(result.begin(),result.end(),[](const auto* a,const auto* b){return a->order!=b->order?a->order<b->order:a->id<b->id;});
    return result;
}
void mode_cards(const Context& c) {
    auto& s=state(); if(!s.cards.handle || s.originals.size()!=3)return;
    for(const auto* definition:plugin_definitions()) {
        const auto& d=*definition;
        auto [it,fresh]=s.mode_cards.try_emplace(d.id);auto& card=it->second;
        if(fresh) {
            const auto& donor=s.originals.at(std::min(d.donor,2u));
            card.tile=make(c,tile_schema,donor.tile.type);c.copy(card.tile,c.address(donor.tile));
            card.category=make(c,category_schema,donor.category.type);c.copy(card.category,c.address(donor.category));
            card.description=make(c,anchor_schema,donor.description.type);c.copy(card.description,c.address(donor.description));
            const auto label=make(c,label_schema,s.source_label.type);c.copy(label,c.address(s.source_label));plain_label(c,label,d.description);
            auto desc=read<Widget>(c.address(c.field(card.description,widget)));desc.data={0,label.handle};c.set(c.field(card.description,widget),desc);
            auto cat=read<Widget>(c.address(c.field(card.category,widget)));cat.data={0,card.description.handle};c.set(c.field(card.category,widget),cat);
            auto mounted=read<Widget>(c.address(c.path(card.tile,{widget,widget})));mounted.data={0,card.category.handle};c.set(c.path(card.tile,{widget,widget}),mounted);
            plain_label(c,c.field(card.category,0x77b8ed0b),d.name);
            auto dimensions=donor.tile_size;dimensions[0]*=.74f;c.set(c.field(card.tile,0x868043e1),dimensions);
            auto description_size=donor.description_size;description_size[0]*=.74f;c.set(c.field(card.description,0x1cff7243),description_size);
            c.set(c.field(card.category,0x495ffd43),donor.icon_width*.74f);c.set(c.field(card.category,0x24fb5ca8),donor.icon_height*.74f);
            logging::log(logging::Level::info,logging::Channel::ui,"Throwdowns: registered native {} card ({}).",d.name,d.id);
        }
        // Reserve callbacks each pass, so reopening never recycles a live card's
        // delegate. Optional artwork leaves the authored donor intact.
        c.text(c.field(primary(c,card.tile),0xb1e14cd6),"");
        c.set(c.field(primary(c,card.tile),0x9c76b86c),callback(c,"mode-open",d.id));
        mode_art(c,std::array{c.field(card.category,0x189084da),c.field(card.category,0xc2917efc)},d.logo);
    }
}

Value picker_logo(const Context& c,Value tile,Value cat,bool stock,const std::string& name) {
    // Private style copies retain the native button/focus graph. Every fill
    // background state is empty. Only the native circular outline changes on
    // hover/focus; artwork and lettering retain their original colors.
    const auto button_style=record_model(c,"ButtonStyleList/TileButton.RoughPartial.NoDefaultBG");
    const auto empty=c.path(button_style,{0x2d393a3a,0xb6c6aace});
    for(const auto key:{0x24996c65U,0x69cf50caU,0xe8f882b7U,0x182004ffU,0xb0db14beU,0xfadedb6aU})
        c.copy(c.path(button_style,{0x2d393a3a,key}),c.address(empty));
    // Hover decoration is optional; failure to stream a style must never
    // remove all custom challenges from the native Throwdown menu.
    try {
        const auto ring=asset(c,"UI/ReSkate/Throwdowns/LogoHoverRing_FillStyle");
        for(const auto key:{0xf622ea66U,0x943abf2dU,0x287f4815U})
            c.set(c.path(button_style,{0x343abac3,key}),ring);
    } catch(const std::exception& e) {
        static bool reported{};
        if(!std::exchange(reported,true))logging::log(logging::Level::warning,logging::Channel::ui,
            "Throwdown hover ring unavailable; retaining the native focus style: {}",e.what());
    }
    for(const auto flag:{0xc02028cfU,0xa45fc55dU,0xb811842aU,0xe69a9d99U})c.set(c.field(button_style,flag),false);
    c.set(c.path(tile,{widget,0x0fb0d794,0x8cc042ef}),Ref{0,button_style.handle});
    c.set(c.path(tile,{widget,0xa04998ca}),false);
    const auto colors=record_ref(c,"InteractivityColorStylesList/InteractiveColors.Core.AllWhite");
    c.set(c.field(button_style,0xcd4a53c0),colors);
    const auto logo=record_model(c,"Throwdown_ModeSelect_ContentResources/LogoTexture",texture_schema);
    if(stock)c.copy(c.field(logo,0x6e469d2a),c.address(c.field(cat,0x189084da)));
    c.set(c.field(logo,0xf524c836),320.f);c.set(c.field(logo,0x6fbd254e),320.f);
    for(const auto flag:{0x81c94d8aU,0x190e039cU})c.set(c.field(logo,flag),false);
    c.set(c.field(logo,0xd1997d0c),false);c.set(c.field(logo,0xcd4a53c0),colors);
    const auto caption=make(c,label_schema,state().source_label.type);c.copy(caption,c.address(state().source_label));
    plain_label(c,caption,name);
    label_styles(c,caption,"TextStylesList/H52-XBold_White_DropShadow75","TextStylesList/H52-XBold_White_DropShadow75");
    const auto outlined_name=record_model(c,"TextStylesList/H52-XBold_White_DropShadow75");
    c.set(c.field(outlined_name,0x51332fc1),asset(c,"UI/Foundations/Fonts/Styles/Dingo_2pxOutline_Aqua_FontEffect"));
    for(const auto key:{0xa1e84eb1U,0x25189231U})c.set(c.field(caption,key),Ref{0,outlined_name.handle});
    c.set(c.field(caption,0x87ad624c),2);c.set(c.field(caption,0x3d8639d0),1); // Center vertically, right aligned.
    c.set(c.field(caption,0xc13ba637),false);c.set(c.field(caption,0xfc53d427),false);
    c.set(c.field(caption,0xcd71a279),true);c.set(c.field(caption,0x3fa0b887),false);
    const auto contents=record_model(c,"Throwdown_ModeSelect_ContentResources/LogoContents",{0x9ade4bc7,208});
    c.set(c.field(contents,0xa98391bd),Widget{asset(c,"UI/ReSkate/Throwdowns/LogoTexture_Widget"),{0,logo.handle}});
    c.set(c.field(contents,0x759c54dd),Widget{asset(c,"UI/ReSkate/Throwdowns/LogoName_Widget"),{0,caption.handle}});
    // The private native blueprint owns fixed regions: logo (52,16,320,320)
    // and transparent, tilted name (52,341,360,56). Neither is measured by a scrolling
    // list or can displace the other when its font/texture finishes loading.
    // Retain the native tile button, focus behavior and its individual action;
    // replace only its display content with the private native presenter.
    c.set(c.path(tile,{widget,widget}),Widget{asset(c,"UI/ReSkate/Throwdowns/LogoCaptionTile_Widget"),{0,contents.handle}});
    c.set(c.field(tile,0x868043e1),std::array<float,2>{424.f,424.f});
    return logo;
}
// The grid: the three stock throwdowns (a plugin mode that replaces one takes its tile), then
// every other plugin mode by order.
void logo_picker(const Context& c) {
    auto& s=state();if(s.originals.size()!=3)return;
    if(!s.picker_grid.handle) {
        s.picker_grid=record_model(c,"Throwdown_ModeSelect_ContentResources/LogoGrid");
        require(read<std::uint32_t>(read<Address>(s.picker_grid.type))==1948428891U,"Throwdown native grid schema differs.");
        c.set(c.field(s.picker_grid,0x55ca89f4),asset(c,"UI/ReSkate/Throwdowns/LogoPickerListItem_Widget"));
        c.set(c.field(s.picker_grid,1913319895U),424.f); // ColumnSize
        c.set(c.field(s.picker_grid,2171233779U),424.f); // RowSize
        c.set(c.field(s.picker_grid,2454275348U),32.f); // ColumnSpacing
        c.set(c.field(s.picker_grid,2923226299U),32.f); // RowSpacing
        c.set(c.field(s.picker_grid,3625715088U),4); // StaticSegmentItemCount
        c.set(c.field(s.picker_grid,3831727006U),0); // RimeUniformGridSegmentCountMode_StaticCount
        c.set(c.field(s.picker_grid,1287003308U),1); // Vertical rows, four columns
        c.set(c.field(s.picker_grid,369153166U),0); // DefaultIndex
        c.set(c.field(s.picker_grid,3523782405U),0); // Inset
        for(const auto flag:{1972029470U,133293336U,3885835458U,3339695017U,235921311U})c.set(c.field(s.picker_grid,flag),false);
        c.set(c.field(s.picker_grid,1163163164U),true); // Clip to the reserved content area.
        c.set(c.field(s.picker_grid,3275567233U),true); // Remember selection on Back.
        s.picker_anchor=record_model(c,"Throwdown_ModeSelect_ContentResources/Throwdown_ModeSelect_LinearList_AnchoredContent",anchor_schema);
        c.set(c.field(s.picker_anchor,widget),Widget{asset(c,"UI/Foundations/Components/Lists/GridList/Widget/GridList_Widget"),{0,s.picker_grid.handle}});
        c.set(c.field(s.picker_anchor,0x1cff7243),std::array<float,2>{1792.f,1336.f});
        c.set(c.field(s.picker_anchor,0x6efc1a61),false);c.set(c.field(s.picker_anchor,0xbe5683d9),false);
        const auto horizontal=c.field(s.picker_anchor,0xde7d30c7);
        c.set(c.field(horizontal,0x99bbed5a),.5f);c.set(c.field(horizontal,0x49943bed),.5f);
        c.set(c.field(horizontal,0x40bfbff4),0.f);c.set(c.field(horizontal,0x1d233266),0.f);
        c.set(c.field(horizontal,0xfec5e0b1),.5f);c.set(c.field(horizontal,0xd01f4a21),1.f);
        for(unsigned i=0;i<3;++i) {
            s.picker_stock[i]=make(c,tile_schema,s.originals[i].tile.type);c.copy(s.picker_stock[i],c.address(s.originals[i].tile));
            const auto names=std::array{"S.K.A.T.E.","Spot Battle","Skate Jam"};
            s.picker_stock_logos[i]=picker_logo(c,s.picker_stock[i],s.originals[i].category,true,names[i]);
        }
    }
    std::vector<Handle> refs;
    // ContentPresenterTileListItem consumes a ContentPresenterTileViewModel,
    // not its TileButtonListItemViewModel wrapper. Both begin at byte zero,
    // but the wrapper adds one entry to the engine's flattened field table.
    // Passing its root shifts every cached input scope by one field, turning
    // ScrollViewModel.Text into the adjacent delegate. Keep the owner alive
    // and publish the typed content-tile field handle for each grid cell.
    const auto append_tile=[&](Value tile) {
        const auto inner=c.field(tile,widget);
        require(read<std::uint32_t>(read<Address>(inner.type))==content_tile_schema.hash &&
                size(inner.type)==content_tile_schema.size,"Throwdown grid tile model differs.");
        refs.push_back(inner.handle);
    };
    const auto append_mode=[&](const throwdowns::Definition& d) {
        auto& card=s.mode_cards.at(d.id);
        // No logo: the donor tile's native artwork, as on the stock cards.
        if(!card.logo.handle)card.logo=picker_logo(c,card.tile,card.category,d.logo.empty(),d.name);
        mode_art(c,std::array{c.field(card.logo,0x6e469d2a)},d.logo);
        append_tile(card.tile);
    };
    const auto modes=plugin_definitions();
    std::set<std::string> placed;
    for(unsigned i=0;i<3;++i) {
        constexpr std::array<std::string_view,3> stock{"skate","spot","jam"};
        const auto replacing=std::find_if(modes.begin(),modes.end(),[&](const auto* d) {
            return d->replaces==stock[i] && s.mode_cards.contains(d->id) && !placed.contains(d->id);
        });
        if(replacing!=modes.end()) {append_mode(**replacing);placed.insert((*replacing)->id);}
        else append_tile(s.picker_stock[i]);
    }
    for(const auto* d:modes)if(!placed.contains(d->id) && s.mode_cards.contains(d->id))append_mode(*d);
    unsigned count{},stride{};const auto previous=c.array(c.field(s.picker_grid,0xf2c90867),64,count,stride);
    require(stride==sizeof(Handle),"Throwdown grid item handle schema differs.");
    if(previous!=std::vector<std::byte>(std::as_bytes(std::span(refs)).begin(),std::as_bytes(std::span(refs)).end()))
        c.array(c.field(s.picker_grid,0xf2c90867),refs);
    // Only replace the mode selector. Native navigation may reuse this page
    // for another view, or temporarily leave its content array empty.
    if(card_list(c,s.page)) {
        const auto slot=mounted_body(c,s.page);if(!slot)return;
        const auto mounted=read<Widget>(c.address(*slot));
        if(mounted.data.handle!=s.picker_anchor.handle)
            c.set(*slot,Widget{asset(c,"UI/Foundations/Templates/Layouts/Anchored/AnchoredContentPresenter_Widget"),{0,s.picker_anchor.handle}});
    }
}
// A game id for a new flag, as the plugin bridge makes them: odd and time-derived.
std::uint32_t new_game_id() {
    const auto now=GetTickCount64();
    return static_cast<std::uint32_t>((now*2654435761ULL)^(now>>16))|1u;
}
// A plugin card was clicked: its mode takes the stock Spot Battle setup page and native flag at
// once (the plugin relabels the page from its setup document and may still refuse with
// flag.leave()), then it hears `open` with the game id.
void mode_open(const std::string& id) {
    const auto* d=throwdowns::registry().find(id);
    if(!d || d->adapter!=throwdowns::Adapter::plugin)return;
    const auto game=new_game_id();
    kit::flag::Options options;
    options.players=static_cast<unsigned>(std::max(1,d->players));
    options.setup_page=true;
    options.cursor_height=d->cursor_height; // the registration's, e.g. a flag that may stand on anything
    // The native registration's limit is 1 s to an hour. A mode's timer of 0 (none) or past an hour
    // keeps its card working: the plugin runs its own clock.
    if(const auto setup=current_setup(d->id);setup && setup->timer.present)
        options.seconds=static_cast<unsigned>(std::clamp(std::round(setup->timer.value),1.f,3600.f));
    kit::set_current(d->id);
    if(!kit::flag::begin(d->id,game,options)) {
        logging::log(logging::Level::info,logging::Channel::ui,
            "Throwdowns: {} not opened: there is no world yet, or another flag is being placed or waits.",d->id);
        return;
    }
    kit::post(d->id,"open",{std::to_string(game)});
    logging::log(logging::Level::info,logging::Channel::ui,"Throwdowns: opened {} (game {}).",d->id,game);
}
