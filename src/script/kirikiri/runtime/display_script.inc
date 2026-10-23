LR"DANGO((function(){
    var cfg=%["active"=>true,"bound"=>false,"serial"=>0,"native"=>void,"conductors"=>[],"layers"=>[],"methods"=>[],"widthMethod"=>void,"parsed"=>0,"located"=>0,"queued"=>0,"dispatched"=>0,"drawn"=>0,"reason"=>''];
    cfg.report=function(){return 'parsed='+this.parsed+';located='+this.located+';queued='+this.queued+';dispatched='+this.dispatched+';drawn='+this.drawn+';reason='+this.reason;} incontextof cfg;
    var reference=new global.Layer(global.kag,global.kag.primaryLayer);
    try {
        reference.visible=false;
        cfg.widthMethod=reference.font.getTextWidth;
        cfg.reference=reference;
    } catch(e) {invalidate reference;throw e;}
    cfg.resetConductor=function(state,armed,preserveTokens){
        state.slot=void;state.source=void;
        state.tag=void;state.execution=void;state.lastDispatch=void;
        if(!preserveTokens) {state.tokens.clear();state.tokenHead=0;}
        state.armed=armed===void?true:armed;
        for(var i=0;i<this.layers.count;i++) {
            var layer=this.layers[i];layer.original='';layer.display='';layer.replaced=false;layer.overflow=false;
        }
        this.serial++;state.serial=this.serial;
    } incontextof cfg;
    cfg.installLifecycle=function(conductor,name){
        var c=conductor.target;
        if(typeof c[name]=='undefined') return;
        var state=%["target"=>c,"name"=>name,"old"=>c[name],"conductor"=>conductor,"cfg"=>this,"wrapper"=>void];
        state.wrapper=function(){
            this.cfg.resetConductor(this.conductor,this.name!='restore' && this.name!='assign',this.name=='goToLabel');
            return (this.old incontextof this.target)(...);
        } incontextof state;
        c[name]=state.wrapper;
        this.methods.add(state);
    } incontextof cfg;
    // 私有票据只关联同次解析产生的字符对象，不向游戏标签或存档增加字段
    cfg.rememberToken=function(state,tag,source,slot){
        if(!this.active || slot===void || typeof tag!='Object' || tag.tagname!='ch' || tag.text!==source) return;
        if(state.tokens.count>state.tokenHead && state.tokens[state.tokens.count-1].tag===tag) return;
        if(state.tokens.count-state.tokenHead>=32768) return;
        if(state.tokenHead>1024) {
            var remaining=[];
            for(var i=state.tokenHead;i<state.tokens.count;i++) remaining.add(state.tokens[i]);
            state.tokens=remaining;state.tokenHead=0;
        }
        state.tokens.add(%["tag"=>tag,"source"=>source,"slot"=>slot]);
        this.queued++;
    } incontextof cfg;
    cfg.takeToken=function(state,tag){
        var token=state.lastDispatch;
        if(token!==void && token.origin===state.serial && token.tag===tag && tag.tagname=='ch' && tag.text===token.source) return token;
        for(var i=state.tokenHead;i<state.tokens.count;i++) {
            token=state.tokens[i];
            if(token!==void && token.tag===tag) {
                for(var j=state.tokenHead;j<=i;j++) state.tokens[j]=void;
                state.tokenHead=i+1;
                if(tag.tagname=='ch' && tag.text===token.source) return token;
                return;
            }
        }
        if(state.tag===tag && tag.tagname=='ch' && tag.text===state.source && state.slot!==void && state.storage===state.target.curStorage && state.line===state.target.curLine && state.position===state.target.curPos && state.sourceLine===state.target.curLineStr) return %["tag"=>tag,"source"=>state.source,"slot"=>state.slot];
    } incontextof cfg;
)DANGO"
LR"DANGO(    cfg.installDispatch=function(state){
        var c=state.target;
        if(typeof c.onTag!='Object') return;
        var dispatch=%["target"=>c,"name"=>'onTag',"old"=>c.onTag,"conductor"=>state,"cfg"=>this,"wrapper"=>void];
        dispatch.wrapper=function(tag){
            var state=this.conductor;var previous=state.execution;
            if(this.cfg.bound && this.cfg.active) {
                try {this.cfg.refresh();} catch(e) {this.cfg.reason=e.message;this.cfg.active=false;}
            }
            var token=this.cfg.active?this.cfg.takeToken(state,tag):void;
            if(token===void && this.cfg.active) token=%["tag"=>tag,"source"=>typeof tag.text=='String'?tag.text:void,"slot"=>void];
            if(token!==void) {
                if(token!==state.lastDispatch) {this.cfg.serial++;token.serial=this.cfg.serial;token.origin=state.serial;}
                state.lastDispatch=token;
                if(token.slot!==void) this.cfg.dispatched++;
            } else state.lastDispatch=void;
            state.execution=token;
            if(this.cfg.bound && this.cfg.active) {
                if(tag.tagname=='r') this.cfg.newline();
                else if(tag.tagname=='p' || tag.tagname=='l' || tag.tagname=='s' || tag.tagname=='cm' || tag.tagname=='er' || tag.tagname=='ct' || tag.tagname=='jump' || tag.tagname=='call' || tag.tagname=='return') {this.cfg.flush();state.armed=true;}
            }
            try {var result=(this.old incontextof this.target)(...);state.execution=previous;return result;}
            catch(e) {state.execution=previous;throw e;}
        } incontextof dispatch;
        c.onTag=dispatch.wrapper;state.dispatch=dispatch;this.methods.add(dispatch);
        var names=['checkTagExtract','enqueueTag'];
        if(typeof c.pendings!='Object' || !(c.pendings instanceof 'Array')) return;
        for(var i=0;i<names.count;i++) {
            var name=names[i];if(typeof c[name]!='Object') continue;
            var method=%["target"=>c,"name"=>name,"old"=>c[name],"conductor"=>state,"cfg"=>this,"wrapper"=>void];
            method.wrapper=function(first,second){
                var state=this.conductor;var c=this.target;
                var begin=c.pendings.count;var source=state.source;var slot=state.slot;
                var valid=this.cfg.active && state.execution===void && state.tag!==void && state.tag.tagname=='ch' && state.tag.text===source && state.storage===c.curStorage && state.line===c.curLine && state.position===c.curPos && state.sourceLine===c.curLineStr;
                if(this.name=='checkTagExtract' && (first!==c || second!==state.tag)) valid=false;
                if(this.name=='enqueueTag' && first!==state.tag && (typeof c.parseMode=='undefined' || !c.parseMode)) valid=false;
                var result=(this.old incontextof c)(...);
                if(valid && slot!==void && c.pendings.count>=begin && c.pendings.count-begin<=128) {
                    var candidate=void;var count=0;
                    for(var j=begin;j<c.pendings.count;j++) {
                        var tag=c.pendings[j];
                        if(typeof tag=='Object' && tag.tagname=='ch' && tag.text===source) {candidate=tag;count++;}
                    }
                    if(count==1) this.cfg.rememberToken(state,candidate,source,slot);
                }
                return result;
            } incontextof method;
            c[name]=method.wrapper;this.methods.add(method);
        }
    } incontextof cfg;
)DANGO"
LR"DANGO(    cfg.installConductor=function(c){
        for(var i=0;i<this.conductors.count;i++) if(this.conductors[i].target===c) return;
        var state=%["target"=>c,"old"=>c.getNextTag,"cfg"=>this,"armed"=>false,"slot"=>void,"source"=>void,"storage"=>void,"line"=>-1,"position"=>-1,"sourceLine"=>void,"serial"=>0,"wrapper"=>void,"tag"=>void,"tokens"=>[],"tokenHead"=>0,"dispatch"=>void,"execution"=>void,"lastDispatch"=>void];
        state.wrapper=function(){
            var c=this.target;
            if(this.cfg.bound && this.cfg.active) {
                try {this.cfg.refresh();} catch(e) {this.cfg.reason=e.message;this.cfg.active=false;}
            }
            var obj=(this.old incontextof c)();
            if(this.dispatch===void && this.cfg.bound && this.cfg.active && obj!==void) {
                if(obj.tagname=='r') this.cfg.newline();
                else if(obj.tagname=='p' || obj.tagname=='l' || obj.tagname=='s' || obj.tagname=='cm' || obj.tagname=='er' || obj.tagname=='ct' || obj.tagname=='jump' || obj.tagname=='call' || obj.tagname=='return') {this.cfg.flush();this.armed=true;}
            }
            this.slot=void;this.source=void;this.tag=obj;
            this.cfg.serial++;this.serial=this.cfg.serial;
            if(this.cfg.active && obj!==void && obj.tagname=='ch') {
                this.cfg.parsed++;
                this.source=obj.text;
                this.storage=c.curStorage;this.line=c.curLine;this.position=c.curPos;this.sourceLine=c.curLineStr;
                this.slot=this.cfg.native(this.storage,this.line,this.position,this.sourceLine,obj.text);
                if(this.slot!==void) this.cfg.located++;
            }
            return obj;
        } incontextof state;
        c.getNextTag=state.wrapper;
        this.conductors.add(state);
        this.installDispatch(state);
        this.installLifecycle(state,'restore');
        this.installLifecycle(state,'clear');
        this.installLifecycle(state,'assign');
        this.installLifecycle(state,'loadScenario');
        this.installLifecycle(state,'goToLabel');
    } incontextof cfg;
    cfg.installLayer=function(layer,owner){
        for(var i=0;i<this.layers.count;i++) if(this.layers[i].target===layer) return;
        var state=%["target"=>layer,"old"=>layer.processCh,"owner"=>owner,"cfg"=>this,"binding"=>void,"completedSerial"=>-1,"capturedSerial"=>-1,"original"=>'',"display"=>'',"replaced"=>false,"overflow"=>false,"wrapper"=>void];
        state.wrapper=function(ch){
            var conductor=this.owner.conductor;
            var state=void;
            if(this.cfg.active) for(var i=0;i<this.cfg.conductors.count;i++) {
                if(this.cfg.conductors[i].target===conductor) {state=this.cfg.conductors[i];break;}
            }
            if(this.cfg.bound && state!==void && !state.armed) return (this.old incontextof this.target)(...);
            var execution=state!==void?state.execution:void;
            var mapped=execution!==void && execution.slot!==void && execution.tag.tagname=='ch' && execution.tag.text===ch && execution.source===ch;
            if(execution===void) {
                if(state!==void && state.dispatch!==void) state=void;
                mapped=state!==void && state.slot!==void && state.source===ch && state.storage===conductor.curStorage && state.line===conductor.curLine && state.position===conductor.curPos && state.sourceLine===conductor.curLineStr;
                execution=state;
            }
            if(typeof this.target.currentRuby!='undefined' && this.target.currentRuby!='') mapped=false;
            if(!mapped || !this.cfg.bindNative(this)) {
                var result=(this.old incontextof this.target)(...);
                if(!result && execution!==void) this.cfg.capture(this,ch,ch,false,execution.serial);
                return result;
            }
            var binding=this.binding;
            binding.source=ch;binding.display=this.completedSerial===execution.serial?'':execution.slot;
            binding.vertical=this.target.vertical;binding.drawCalls=0;binding.enabled=true;
            try {
                for(var i=0;i<binding.parts.count;i++) binding.faces.add(binding.parts[i].font.face);
                for(var i=0;i<binding.parts.count;i++) binding.parts[i].font.face=binding.vertical?'@NSimSun':'NSimSun';
                var result=(this.old incontextof this.target)(...);
                this.cfg.finishNative(binding);
                if(binding.drawCalls>0) this.completedSerial=execution.serial;
                if(binding.drawCalls>0) this.cfg.drawn++;
                if(!result) this.cfg.capture(this,ch,binding.drawCalls>0?binding.display:ch,binding.drawCalls>0,execution.serial);
                return result;
            } catch(e) {this.cfg.finishNative(binding);if(binding.drawCalls>0) this.completedSerial=execution.serial;throw e;}
        } incontextof state;
        layer.processCh=state.wrapper;
        this.layers.add(state);
    } incontextof cfg;
)DANGO"
LR"DANGO(    cfg.capture=function(layer,original,display,replaced,serial){
        if(!this.bound || !this.active || this.native===void || layer.overflow || layer.owner.current!==layer.target || layer.capturedSerial===serial) return;
        if(typeof original!='String' || typeof display!='String') return;
        layer.capturedSerial=serial;
        if(layer.original.length+original.length>32768 || layer.display.length+display.length>32768) {layer.original='';layer.display='';layer.replaced=false;layer.overflow=true;return;}
        layer.original+=original;layer.display+=display;layer.replaced=layer.replaced || replaced;
    } incontextof cfg;
    cfg.newline=function(){
        for(var i=0;i<this.layers.count;i++) {
            var layer=this.layers[i];
            if(layer.original!='') {layer.original+='\n';layer.display+='\n';}
        }
    } incontextof cfg;
    cfg.flush=function(){
        for(var i=0;i<this.layers.count;i++) {
            var layer=this.layers[i];
            if(layer.original!='') this.native(layer.original,layer.display,+layer.replaced,this.serial);
            layer.original='';layer.display='';layer.replaced=false;layer.overflow=false;
        }
    } incontextof cfg;
)DANGO"
LR"DANGO(    cfg.releaseNative=function(state){
        var binding=state.binding;
        if(binding===void) return;
        this.finishNative(binding);
        for(var i=0;i<binding.parts.count;i++) {
            var part=binding.parts[i];
            if(isvalid part.widthTarget && typeof part.widthWrapper=='Object' && part.widthTarget[part.widthName]===part.widthWrapper) part.widthTarget[part.widthName]=part.widthOld;
            if(isvalid part.drawTarget && typeof part.drawWrapper=='Object' && part.drawTarget[part.drawName]===part.drawWrapper) part.drawTarget[part.drawName]=part.drawOld;
            part.widthWrapper=void;part.drawWrapper=void;part.widthProxy=void;part.drawProxy=void;
        }
        binding.parts.clear();
        state.binding=void;
    } incontextof cfg;
    cfg.finishNative=function(binding){
        binding.enabled=false;
        for(var i=binding.faces.count-1;i>=0;i--) {
            if(isvalid binding.parts[i].font) binding.parts[i].font.face=binding.faces[i];
            binding.faces.erase(i);
        }
    } incontextof cfg;
    cfg.bindNative=function(state){
        var line=state.target.lineLayer;
        if(state.binding!==void && state.binding.parts[0].line===line) {
            for(var i=0;i<state.binding.parts.count;i++) {
                var part=state.binding.parts[i];
                if(part.line!==line || part.font!==line.font || part.widthTarget[part.widthName]!==part.widthWrapper || part.drawTarget[part.drawName]!==part.drawWrapper) return false;
                if(part.drawTarget!==line && (line._preRenderFontEx!==part.drawTarget || part.drawTarget.owner!==line)) return false;
                if(i+1<state.binding.parts.count) line=line._divlay;
                else if(typeof line._divlay!='undefined' && line._divlay!==void) return false;
            }
            return true;
        }
        this.releaseNative(state);
        var parts=this.resolve(this.native,this.widthMethod,line);
        if(parts===void) return false;
        var binding=%["parts"=>parts,"faces"=>[],"enabled"=>false,"source"=>void,"display"=>void,"vertical"=>false,"drawCalls"=>0];
        state.binding=binding;
        for(var i=0;i<parts.count;i++) {
            var part=parts[i];part.binding=binding;
            part.widthOld=part.widthTarget[part.widthName];part.drawOld=part.drawTarget[part.drawName];
            part.widthProxy=this.native(part.widthOld,binding,0);
            part.drawProxy=this.native(part.drawOld,binding,2);
            part.widthWrapper=function(text){
                if(!this.binding.enabled || text!==this.binding.source) return (this.widthOld incontextof this.font)(...);
                return (this.widthProxy incontextof this.font)(...);
            } incontextof part;
            part.drawWrapper=function(x,y,text){
                if(!this.binding.enabled || text!==this.binding.source) return (this.drawOld incontextof this.line)(...);
                var result=(this.drawProxy incontextof this.line)(...);
                this.binding.drawCalls++;return result;
            } incontextof part;
            part.widthTarget[part.widthName]=part.widthWrapper;part.drawTarget[part.drawName]=part.drawWrapper;
        }
        return true;
    } incontextof cfg;
)DANGO"
LR"DANGO(    cfg.refresh=function(){
        var conductors=[global.kag.mainConductor,global.kag.extraConductor];
        for(var i=0;i<conductors.count;i++) {
            var c=conductors[i];var found=false;
            if(!(c instanceof 'KAGParser') || !this.native(c,2) || c.owner!==global.kag) throw new global.Exception('parser-layout');
            for(var j=0;j<this.conductors.count;j++) if(this.conductors[j].target===c) {
                if(c.getNextTag!==this.conductors[j].wrapper) throw new global.Exception('parser-changed');
                var state=this.conductors[j];
                if(state.dispatch!==void && c.onTag!==state.dispatch.wrapper) throw new global.Exception('dispatch-changed');
                for(var k=0;k<this.methods.count;k++) {
                    var method=this.methods[k];
                    if(method.target===c && (method.name=='checkTagExtract' || method.name=='enqueueTag') && c[method.name]!==method.wrapper) throw new global.Exception('queue-changed');
                }
                found=true;break;
            }
            if(!found) {
                if(c.getNextTag!==(global.KAGParser.getNextTag incontextof c)) throw new global.Exception('parser-entry');
                this.installConductor(c);
            }
        }
        var pages=[global.kag.fore,global.kag.back];
        for(var i=0;i<pages.count;i++) {
            var messages=pages[i].messages;
            if(messages.count<1 || messages.count>32) throw new global.Exception('layer-count');
            for(var j=0;j<messages.count;j++) {
                var layer=messages[j];var found=false;
                if(typeof layer.processCh!='Object') throw new global.Exception('message-entry');
                for(var k=0;k<this.layers.count;k++) if(this.layers[k].target===layer) {
                    if(layer.processCh!==this.layers[k].wrapper) throw new global.Exception('message-changed');
                    found=true;break;
                }
                if(!found) {
                    if(this.resolve(this.native,this.widthMethod,layer.lineLayer)===void) throw new global.Exception('native-entry');
                    this.installLayer(layer,global.kag);
                }
            }
        }
        for(var i=this.layers.count-1;i>=0;i--) if(!isvalid this.layers[i].target) {
            this.releaseNative(this.layers[i]);this.layers.erase(i);
        }
    } incontextof cfg;
    cfg.activate=function(){
        if(this.bound) throw new global.Exception('already-active');
        try {this.refresh();this.active=true;this.bound=true;return 1;}
        catch(e) {this.detach();throw e;}
    } incontextof cfg;
    cfg.detach=function(){
        this.active=false;this.bound=false;
        for(var i=0;i<this.conductors.count;i++) {
            var state=this.conductors[i];
            state.tokens.clear();state.lastDispatch=void;state.execution=void;
            if(isvalid state.target && state.target.getNextTag===state.wrapper) state.target.getNextTag=state.old;
        }
        for(var i=0;i<this.layers.count;i++) {
            var state=this.layers[i];
            this.releaseNative(state);
            if(isvalid state.target && state.target.processCh===state.wrapper) state.target.processCh=state.old;
        }
        for(var i=0;i<this.methods.count;i++) {
            var state=this.methods[i];
            if(isvalid state.target && state.target[state.name]===state.wrapper) state.target[state.name]=state.old;
        }
        this.conductors.clear();this.layers.clear();this.methods.clear();this.native=void;this.widthMethod=void;
    } incontextof cfg;
    return cfg;
})())DANGO"
